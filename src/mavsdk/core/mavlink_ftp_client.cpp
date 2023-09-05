#include "mavlink_ftp_client.h"
#include "system_impl.h"
#include "plugin_base.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

#include "crc32.h"

namespace mavsdk {

namespace fs = std::filesystem;

MavlinkFtpClient::MavlinkFtpClient(SystemImpl& system_impl) : _system_impl(system_impl)
{
    if (const char* env_p = std::getenv("MAVSDK_FTP_DEBUGGING")) {
        if (std::string(env_p) == "1") {
            LogDebug() << "Ftp debugging is on.";
            _debugging = true;
        }
    }

    _system_impl.register_mavlink_message_handler(
        MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL,
        [this](const mavlink_message_t& message) { process_mavlink_ftp_message(message); },
        this);
}

MavlinkFtpClient::~MavlinkFtpClient() {}

void MavlinkFtpClient::do_work()
{
    LockedQueue<Work>::Guard work_queue_guard(_work_queue);

    auto work = work_queue_guard.get_front();
    if (!work) {
        return;
    }

    if (work->started) {
        return;
    }
    work->started = true;

    // We're mainly starting the process here. After that, it continues
    // based on returned acks or timeouts.

    std::visit(
        overloaded{
            [&](DownloadItem& item) {
                if (!download_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](UploadItem& item) {
                if (!upload_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](RemoveItem& item) {
                if (!remove_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](RenameItem& item) {
                if (!rename_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](CreateDirItem& item) {
                if (!create_dir_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](RemoveDirItem& item) {
                if (!remove_dir_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](CompareFilesItem& item) {
                if (!compare_files_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            },
            [&](ListDirItem& item) {
                if (!list_dir_start(*work, item)) {
                    work_queue_guard.pop_front();
                }
            }},
        work->item);
}

void MavlinkFtpClient::process_mavlink_ftp_message(const mavlink_message_t& msg)
{
    mavlink_file_transfer_protocol_t ftp_req;
    mavlink_msg_file_transfer_protocol_decode(&msg, &ftp_req);

    if (ftp_req.target_system != 0 && ftp_req.target_system != _system_impl.get_own_system_id()) {
        LogWarn() << "Received FTP with wrong target system ID!";
        return;
    }

    if (ftp_req.target_component != 0 &&
        ftp_req.target_component != _system_impl.get_own_component_id()) {
        LogWarn() << "Received FTP with wrong target component ID!";
        return;
    }

    PayloadHeader* payload = reinterpret_cast<PayloadHeader*>(&ftp_req.payload[0]);

    if (payload->size > max_data_length) {
        LogWarn() << "Received FTP payload with invalid size";
        return;
    } else {
        if (_debugging) {
            LogDebug() << "FTP: opcode: " << (int)payload->opcode
                       << ", size: " << (int)payload->size << ", offset: " << (int)payload->offset
                       << ", seq: " << payload->seq_number;
        }
    }

    LockedQueue<Work>::Guard work_queue_guard(_work_queue);

    auto work = work_queue_guard.get_front();
    if (!work) {
        return;
    }

    if (work->last_opcode != payload->req_opcode) {
        // Ignore
        LogWarn() << "Ignore: last: " << (int)work->last_opcode
                  << ", req: " << (int)payload->req_opcode;
        return;
    }
    if (work->last_received_seq_number != 0 &&
        work->last_received_seq_number == payload->seq_number) {
        // We have already seen this ack/nak.
        LogWarn() << "Already seen";
        return;
    }

    std::visit(
        overloaded{
            [&](DownloadItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_OPEN_FILE_RO ||
                        payload->req_opcode == CMD_READ_FILE) {
                        // Whenever we do get an ack,
                        // reset the retry counter.
                        work->retries = RETRIES;

                        if (!download_continue(*work, item, payload)) {
                            stop_timer();
                            work_queue_guard.pop_front();
                        }
                    } else if (payload->req_opcode == CMD_TERMINATE_SESSION) {
                        stop_timer();
                        item.ofstream.close();
                        item.callback(ClientResult::Success, {});
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload), {});
                    work_queue_guard.pop_front();
                }
            },
            [&](UploadItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_CREATE_FILE ||
                        payload->req_opcode == CMD_OPEN_FILE_WO ||
                        payload->req_opcode == CMD_WRITE_FILE) {
                        // Whenever we do get an ack,
                        // reset the retry counter.
                        work->retries = RETRIES;

                        if (!upload_continue(*work, item)) {
                            stop_timer();
                            work_queue_guard.pop_front();
                        }
                    } else if (payload->req_opcode == CMD_TERMINATE_SESSION) {
                        stop_timer();
                        item.ifstream.close();
                        item.callback(ClientResult::Success, {});
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload), {});
                    work_queue_guard.pop_front();
                }
            },
            [&](RemoveItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_REMOVE_FILE) {
                        stop_timer();
                        item.callback(ClientResult::Success);
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload));
                    work_queue_guard.pop_front();
                }
            },
            [&](RenameItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_RENAME) {
                        stop_timer();
                        item.callback(ClientResult::Success);
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload));
                    work_queue_guard.pop_front();
                }
            },
            [&](CreateDirItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_CREATE_DIRECTORY) {
                        stop_timer();
                        item.callback(ClientResult::Success);
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload));
                    work_queue_guard.pop_front();
                }
            },
            [&](RemoveDirItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_REMOVE_DIRECTORY) {
                        stop_timer();
                        item.callback(ClientResult::Success);
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload));
                    work_queue_guard.pop_front();
                }
            },
            [&](CompareFilesItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_CALC_FILE_CRC32) {
                        stop_timer();
                        uint32_t remote_crc = *reinterpret_cast<uint32_t*>(payload->data);
                        item.callback(ClientResult::Success, remote_crc == item.local_crc);
                        work_queue_guard.pop_front();

                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    item.callback(result_from_nak(payload), false);
                    work_queue_guard.pop_front();
                }
            },
            [&](ListDirItem& item) {
                if (payload->opcode == RSP_ACK) {
                    if (payload->req_opcode == CMD_LIST_DIRECTORY) {
                        // Whenever we do get an ack, reset the retry counter.
                        work->retries = RETRIES;

                        if (!list_dir_continue(*work, item, payload)) {
                            stop_timer();
                            work_queue_guard.pop_front();
                        }
                    } else {
                        LogWarn() << "Unexpected ack";
                    }

                } else if (payload->opcode == RSP_NAK) {
                    stop_timer();
                    if (payload->data[0] == ERR_EOF) {
                        std::sort(item.dirs.begin(), item.dirs.end());
                        item.callback(ClientResult::Success, item.dirs);
                    } else {
                        item.callback(result_from_nak(payload), {});
                    }
                    work_queue_guard.pop_front();
                }
            }},
        work->item);

    work->last_received_seq_number = payload->seq_number;
}

bool MavlinkFtpClient::download_start(Work& work, DownloadItem& item)
{
    fs::path local_path = fs::path(item.local_folder) / fs::path(item.remote_path).filename();

    LogDebug() << "Trying to open write to local path: " << local_path.string();
    item.ofstream.open(local_path, std::fstream::trunc | std::fstream::binary);
    if (!item.ofstream) {
        LogErr() << "Could not open it!";
        item.callback(ClientResult::FileIoError, {});
        return false;
    }

    item.last_progress_percentage = -1;

    work.last_opcode = CMD_OPEN_FILE_RO;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(
        reinterpret_cast<char*>(work.payload.data), item.remote_path.c_str(), max_data_length - 1);
    work.payload.size = item.remote_path.length() + 1;

    start_timer();
    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::download_continue(Work& work, DownloadItem& item, PayloadHeader* payload)
{
    if (payload->req_opcode == CMD_OPEN_FILE_RO) {
        item.file_size = *(reinterpret_cast<uint32_t*>(payload->data));

        if (_debugging) {
            LogWarn() << "Download continue, got file size: " << item.file_size;
        }

    } else if (payload->req_opcode == CMD_READ_FILE) {
        if (_debugging) {
            LogWarn() << "Download continue, write: " << std::to_string(payload->size);
        }

        if (item.bytes_transferred < item.file_size) {
            item.ofstream.write(reinterpret_cast<const char*>(payload->data), payload->size);
            if (!item.ofstream) {
                item.callback(ClientResult::FileIoError, {});
                return false;
            }
            item.bytes_transferred += payload->size;

            if (_debugging) {
                LogDebug() << "Written " << item.bytes_transferred << " of " << item.file_size
                           << " bytes";
            }
        }
        item.callback(
            ClientResult::Next,
            ProgressData{
                static_cast<uint32_t>(item.bytes_transferred),
                static_cast<uint32_t>(item.file_size)});
    }

    if (item.bytes_transferred < item.file_size) {
        work.last_opcode = CMD_READ_FILE;
        work.payload = {};
        work.payload.seq_number = work.last_sent_seq_number++;
        work.payload.session = _session;
        work.payload.opcode = work.last_opcode;
        work.payload.offset = item.bytes_transferred;
        work.payload.size =
            std::min(static_cast<size_t>(max_data_length), item.file_size - item.bytes_transferred);
        LogWarn() << "Request size: " << std::to_string(work.payload.size) << " of left "
                  << int(item.file_size - item.bytes_transferred);
        start_timer();
        send_mavlink_ftp_message(work.payload);

        return true;
    } else {
        if (_debugging) {
            LogDebug() << "All bytes written, terminating session";
        }

        // Final step
        work.last_opcode = CMD_TERMINATE_SESSION;

        work.payload = {};
        work.payload.seq_number = work.last_sent_seq_number++;
        work.payload.session = _session;

        work.payload.opcode = work.last_opcode;
        work.payload.offset = 0;
        work.payload.size = 0;

        start_timer();
        send_mavlink_ftp_message(work.payload);
    }

    return true;
}

bool MavlinkFtpClient::upload_start(Work& work, UploadItem& item)
{
    std::error_code ec;
    if (!fs::exists(item.local_file_path, ec)) {
        item.callback(ClientResult::FileDoesNotExist, {});
        return false;
    }

    item.ifstream.open(item.local_file_path, std::fstream::binary);
    if (!item.ifstream) {
        item.callback(ClientResult::FileIoError, {});
        return false;
    }

    item.file_size = fs::file_size(item.local_file_path, ec);
    if (ec) {
        LogWarn() << "Could not get file size of '" << item.local_file_path
                  << "': " << ec.message();
        return false;
    }

    fs::path remote_file_path =
        fs::path(item.remote_folder) / fs::path(item.local_file_path).filename();

    if (remote_file_path.string().size() >= max_data_length) {
        item.callback(ClientResult::InvalidParameter, {});
        return false;
    }

    work.last_opcode = CMD_CREATE_FILE;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(
        reinterpret_cast<char*>(work.payload.data),
        remote_file_path.string().c_str(),
        max_data_length - 1);
    work.payload.size = remote_file_path.string().size() + 1;

    start_timer();
    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::upload_continue(Work& work, UploadItem& item)
{
    if (item.bytes_transferred < item.file_size) {
        work.last_opcode = CMD_WRITE_FILE;

        work.payload = {};
        work.payload.seq_number = work.last_sent_seq_number++;
        work.payload.session = _session;

        work.payload.opcode = work.last_opcode;
        work.payload.offset = item.bytes_transferred;

        std::size_t bytes_to_read =
            std::min(item.file_size - item.bytes_transferred, std::size_t(max_data_length));

        item.ifstream.read(reinterpret_cast<char*>(work.payload.data), bytes_to_read);

        // Get the number of bytes actually read.
        int bytes_read = item.ifstream.gcount();

        if (!item.ifstream) {
            item.callback(ClientResult::FileIoError, {});
            return false;
        }

        if (_debugging) {
            LogDebug() << "Read " << bytes_read << " bytes";
        }

        work.payload.size = bytes_read;
        item.bytes_transferred += bytes_read;

        start_timer();
        send_mavlink_ftp_message(work.payload);

    } else {
        // Final step
        work.last_opcode = CMD_TERMINATE_SESSION;

        work.payload = {};
        work.payload.seq_number = work.last_sent_seq_number++;
        work.payload.session = _session;

        work.payload.opcode = work.last_opcode;
        work.payload.offset = 0;
        work.payload.size = 0;

        start_timer();
        send_mavlink_ftp_message(work.payload);
    }

    item.callback(
        ClientResult::Next,
        ProgressData{
            static_cast<uint32_t>(item.bytes_transferred), static_cast<uint32_t>(item.file_size)});

    return true;
}

bool MavlinkFtpClient::remove_start(Work& work, RemoveItem& item)
{
    if (item.path.length() >= max_data_length) {
        item.callback(ClientResult::InvalidParameter);
        return false;
    }

    work.last_opcode = CMD_REMOVE_FILE;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(reinterpret_cast<char*>(work.payload.data), item.path.c_str(), max_data_length - 1);
    work.payload.size = item.path.length() + 1;

    start_timer();
    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::rename_start(Work& work, RenameItem& item)
{
    if (item.from_path.length() + item.to_path.length() + 1 >= max_data_length) {
        item.callback(ClientResult::InvalidParameter);
        return false;
    }

    work.last_opcode = CMD_RENAME;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(
        reinterpret_cast<char*>(work.payload.data), item.from_path.c_str(), max_data_length - 1);
    work.payload.size = item.from_path.length() + 1;
    strncpy(
        reinterpret_cast<char*>(&work.payload.data[work.payload.size]),
        item.to_path.c_str(),
        max_data_length - work.payload.size);
    work.payload.size += item.to_path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::create_dir_start(Work& work, CreateDirItem& item)
{
    if (item.path.length() + 1 >= max_data_length) {
        item.callback(ClientResult::InvalidParameter);
        return false;
    }

    work.last_opcode = CMD_CREATE_DIRECTORY;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(reinterpret_cast<char*>(work.payload.data), item.path.c_str(), max_data_length - 1);
    work.payload.size = item.path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::remove_dir_start(Work& work, RemoveDirItem& item)
{
    if (item.path.length() + 1 >= max_data_length) {
        item.callback(ClientResult::InvalidParameter);
        return false;
    }

    work.last_opcode = CMD_REMOVE_DIRECTORY;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(reinterpret_cast<char*>(work.payload.data), item.path.c_str(), max_data_length - 1);
    work.payload.size = item.path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::compare_files_start(Work& work, CompareFilesItem& item)
{
    if (item.remote_path.length() + 1 >= max_data_length) {
        item.callback(ClientResult::InvalidParameter, false);
        return false;
    }

    auto result_local = calc_local_file_crc32(item.local_path, item.local_crc);
    if (result_local != ClientResult::Success) {
        item.callback(result_local, false);
        return false;
    }

    work.last_opcode = CMD_CALC_FILE_CRC32;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(
        reinterpret_cast<char*>(work.payload.data), item.remote_path.c_str(), max_data_length - 1);
    work.payload.size = item.remote_path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::list_dir_start(Work& work, ListDirItem& item)
{
    if (item.path.length() + 1 >= max_data_length) {
        item.callback(ClientResult::InvalidParameter, {});
        return false;
    }

    work.last_opcode = CMD_LIST_DIRECTORY;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = 0;
    strncpy(reinterpret_cast<char*>(work.payload.data), item.path.c_str(), max_data_length - 1);
    work.payload.size = item.path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

bool MavlinkFtpClient::list_dir_continue(Work& work, ListDirItem& item, PayloadHeader* payload)
{
    if (_debugging) {
        LogDebug() << "List dir response received, got " << (int)payload->size << " chars";
    }

    if (payload->size > max_data_length) {
        LogWarn() << "Received FTP payload with invalid size";
        return false;
    }

    // Make sure there is a zero termination.
    payload->data[payload->size - 1] = '\0';

    size_t i = 0;
    while (i + 1 < payload->size) {
        const int entry_len = std::strlen(reinterpret_cast<char*>(&payload->data[i]));

        std::string entry;
        entry.resize(entry_len);
        std::memcpy(entry.data(), &payload->data[i], entry_len);

        i += entry_len + 1;

        ++item.offset;

        if (entry[0] == 'S') {
            // Skip skip for now
            continue;
        }

        item.dirs.push_back(entry);
    }

    work.last_opcode = CMD_LIST_DIRECTORY;
    work.payload = {};
    work.payload.seq_number = work.last_sent_seq_number++;
    work.payload.session = 0;
    work.payload.opcode = work.last_opcode;
    work.payload.offset = item.offset;
    strncpy(reinterpret_cast<char*>(work.payload.data), item.path.c_str(), max_data_length - 1);
    work.payload.size = item.path.length() + 1;
    start_timer();

    send_mavlink_ftp_message(work.payload);

    return true;
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::result_from_nak(PayloadHeader* payload)
{
    ServerResult sr = static_cast<ServerResult>(payload->data[0]);
    LogWarn() << "Got nack: " << std::to_string(sr);

    // PX4 Mavlink FTP returns "File doesn't exist" this way
    if (sr == ServerResult::ERR_FAIL_ERRNO && payload->data[1] == ENOENT) {
        sr = ServerResult::ERR_FAIL_FILE_DOES_NOT_EXIST;
    }

    return translate(sr);
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::translate(ServerResult result)
{
    switch (result) {
        case ServerResult::SUCCESS:
            return ClientResult::Success;
        case ServerResult::ERR_TIMEOUT:
            return ClientResult::Timeout;
        case ServerResult::ERR_FILE_IO_ERROR:
            return ClientResult::FileIoError;
        case ServerResult::ERR_FAIL_FILE_EXISTS:
            return ClientResult::FileExists;
        case ServerResult::ERR_FAIL_FILE_PROTECTED:
            return ClientResult::FileProtected;
        case ServerResult::ERR_UNKOWN_COMMAND:
            return ClientResult::Unsupported;
        case ServerResult::ERR_FAIL_FILE_DOES_NOT_EXIST:
            return ClientResult::FileDoesNotExist;
        default:
            return ClientResult::ProtocolError;
    }
}

void MavlinkFtpClient::download_async(
    const std::string& remote_path, const std::string& local_folder, DownloadCallback callback)
{
    auto item = DownloadItem{};
    item.remote_path = remote_path;
    item.local_folder = local_folder;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

void MavlinkFtpClient::upload_async(
    const std::string& local_file_path, const std::string& remote_folder, UploadCallback callback)
{
    auto item = UploadItem{};
    item.local_file_path = local_file_path;
    item.remote_folder = remote_folder;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

std::pair<MavlinkFtpClient::ClientResult, std::vector<std::string>>
MavlinkFtpClient::list_directory(const std::string& path)
{
    std::promise<std::pair<ClientResult, std::vector<std::string>>> prom;
    auto fut = prom.get_future();

    list_directory_async(
        path, [&prom](const ClientResult result, const std::vector<std::string> dirs) {
            prom.set_value(std::make_pair(result, dirs));
        });

    return fut.get();
}

void MavlinkFtpClient::list_directory_async(const std::string& path, ListDirectoryCallback callback)
{
    auto item = ListDirItem{};
    item.path = path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::create_directory(const std::string& path)
{
    std::promise<ClientResult> prom;
    auto fut = prom.get_future();

    create_directory_async(path, [&prom](const ClientResult result) { prom.set_value(result); });

    return fut.get();
}

void MavlinkFtpClient::create_directory_async(const std::string& path, ResultCallback callback)
{
    auto item = CreateDirItem{};
    item.path = path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::remove_directory(const std::string& path)
{
    std::promise<ClientResult> prom;
    auto fut = prom.get_future();

    remove_directory_async(path, [&prom](const ClientResult result) { prom.set_value(result); });

    return fut.get();
}

void MavlinkFtpClient::remove_directory_async(const std::string& path, ResultCallback callback)
{
    auto item = RemoveDirItem{};
    item.path = path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::remove_file(const std::string& path)
{
    std::promise<ClientResult> prom;
    auto fut = prom.get_future();

    remove_file_async(path, [&prom](const ClientResult result) { prom.set_value(result); });

    return fut.get();
}

void MavlinkFtpClient::remove_file_async(const std::string& path, ResultCallback callback)
{
    auto item = RemoveItem{};
    item.path = path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

MavlinkFtpClient::ClientResult
MavlinkFtpClient::rename(const std::string& from_path, const std::string& to_path)
{
    std::promise<ClientResult> prom;
    auto fut = prom.get_future();

    rename_async(
        from_path, to_path, [&prom](const ClientResult result) { prom.set_value(result); });

    return fut.get();
}

void MavlinkFtpClient::rename_async(
    const std::string& from_path, const std::string& to_path, ResultCallback callback)
{
    auto item = RenameItem{};
    item.from_path = from_path;
    item.to_path = to_path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

std::pair<MavlinkFtpClient::ClientResult, bool>
MavlinkFtpClient::are_files_identical(const std::string& local_path, const std::string& remote_path)
{
    std::promise<std::pair<ClientResult, bool>> prom;
    auto fut = prom.get_future();

    are_files_identical_async(
        local_path, remote_path, [&prom](const ClientResult result, const bool are_identical) {
            prom.set_value(std::make_pair(result, are_identical));
        });

    return fut.get();
}

void MavlinkFtpClient::are_files_identical_async(
    const std::string& local_path,
    const std::string& remote_path,
    AreFilesIdenticalCallback callback)
{
    auto item = CompareFilesItem{};
    item.local_path = local_path;
    item.remote_path = remote_path;
    item.callback = callback;
    auto new_work = Work{std::move(item)};

    _work_queue.push_back(std::make_shared<Work>(std::move(new_work)));
}

void MavlinkFtpClient::send_mavlink_ftp_message(const PayloadHeader& payload)
{
    mavlink_message_t message;
    mavlink_msg_file_transfer_protocol_pack(
        _system_impl.get_own_system_id(),
        _system_impl.get_own_component_id(),
        &message,
        _network_id,
        _system_impl.get_system_id(),
        get_target_component_id(),
        reinterpret_cast<const uint8_t*>(&payload));
    _system_impl.send_message(message);
}

void MavlinkFtpClient::start_timer()
{
    _system_impl.unregister_timeout_handler(_timeout_cookie);
    _system_impl.register_timeout_handler(
        [this]() { timeout(); }, _system_impl.timeout_s(), &_timeout_cookie);
}

void MavlinkFtpClient::stop_timer()
{
    _system_impl.unregister_timeout_handler(_timeout_cookie);
}

void MavlinkFtpClient::timeout()
{
    if (_debugging) {
        LogDebug() << "Timeout!";
    }

    LockedQueue<Work>::Guard work_queue_guard(_work_queue);
    auto work = work_queue_guard.get_front();
    if (!work) {
        return;
    }

    std::visit(
        overloaded{
            [&](DownloadItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout, {});
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](UploadItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout, {});
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](RemoveItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout);
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](RenameItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout);
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](CreateDirItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout);
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](RemoveDirItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout);
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](CompareFilesItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout, false);
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            },
            [&](ListDirItem& item) {
                if (--work->retries == 0) {
                    item.callback(ClientResult::Timeout, {});
                    work_queue_guard.pop_front();
                    return;
                }
                if (_debugging) {
                    LogDebug() << "Retries left: " << work->retries;
                }

                start_timer();
                send_mavlink_ftp_message(work->payload);
            }},
        work->item);
}

MavlinkFtpClient::ClientResult
MavlinkFtpClient::calc_local_file_crc32(const std::string& path, uint32_t& csum)
{
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return ClientResult::FileDoesNotExist;
    }

    std::ifstream stream(path, std::fstream::binary);
    if (!stream) {
        return ClientResult::FileIoError;
    }

    // Read whole file in buffer size chunks
    Crc32 checksum;
    uint8_t buffer[4096];
    std::streamsize bytes_read;

    do {
        stream.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
        bytes_read = stream.gcount(); // Get the number of bytes actually read
        checksum.add(reinterpret_cast<const uint8_t*>(buffer), bytes_read);
    } while (bytes_read > 0);

    csum = checksum.get();

    return ClientResult::Success;
}

uint8_t MavlinkFtpClient::get_our_compid()
{
    return _system_impl.get_own_component_id();
}

uint8_t MavlinkFtpClient::get_target_component_id()
{
    return _target_component_id_set ? _target_component_id : _system_impl.get_autopilot_id();
}

MavlinkFtpClient::ClientResult MavlinkFtpClient::set_target_compid(uint8_t component_id)
{
    _target_component_id = component_id;
    _target_component_id_set = true;
    return ClientResult::Success;
}

std::ostream& operator<<(std::ostream& str, MavlinkFtpClient::ClientResult const& result)
{
    switch (result) {
        default:
            // Fallthrough
        case MavlinkFtpClient::ClientResult::Unknown:
            return str << "Unknown";
        case MavlinkFtpClient::ClientResult::Success:
            return str << "Success";
        case MavlinkFtpClient::ClientResult::Next:
            return str << "Next";
        case MavlinkFtpClient::ClientResult::Timeout:
            return str << "Timeout";
        case MavlinkFtpClient::ClientResult::Busy:
            return str << "Busy";
        case MavlinkFtpClient::ClientResult::FileIoError:
            return str << "FileIoError";
        case MavlinkFtpClient::ClientResult::FileExists:
            return str << "FileExists";
        case MavlinkFtpClient::ClientResult::FileDoesNotExist:
            return str << "FileDoesNotExist";
        case MavlinkFtpClient::ClientResult::FileProtected:
            return str << "FileProtected";
        case MavlinkFtpClient::ClientResult::InvalidParameter:
            return str << "InvalidParameter";
        case MavlinkFtpClient::ClientResult::Unsupported:
            return str << "Unsupported";
        case MavlinkFtpClient::ClientResult::ProtocolError:
            return str << "ProtocolError";
        case MavlinkFtpClient::ClientResult::NoSystem:
            return str << "NoSystem";
    }
}

} // namespace mavsdk
