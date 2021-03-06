#include "client.h"
#include "common.h"
#include <plog/Log.h>
#include <cassert>
#include <thread>
#include <json11.hpp>

using namespace json11;
using namespace swechat;

ChatClient* ChatClient::ins = nullptr;

void ChatClient::createInstance()
{
    LOG_ERROR_IF(ins != nullptr) << "Already created chat client instance";
    assert(ins == nullptr);
    ins = new ChatClient();
}

ChatClient* ChatClient::instance()
{
    return ins;
}

ChatClient::ChatClient()
{
    UnbindMsg();
    UnbindError();
    UnbindUsers();
    UnbindFriends();
    UnbindChatMsg();
    UnbindChatMsgs();

    io_thread = thread([=]() {
        while(true)
        {
            auto task = task_que.Pop();
            task();
        }
    });

    com_file_client = make_shared<FileClient>();
}

void ChatClient::BindMsg(string_cb_t msg_cb)
{
    this->msg_cb = msg_cb;
}
void ChatClient::UnbindMsg()
{
    this->msg_cb = [](auto){};
}
void ChatClient::BindError(string_cb_t err_cb)
{
    this->err_cb = err_cb;
}
void ChatClient::UnbindError()
{
    this->err_cb = [](auto){};
}
void ChatClient::BindUsers(users_cb_t users_cb)
{
    this->users_cb = users_cb;
}
void ChatClient::UnbindUsers()
{
    this->users_cb = [](auto){};
}
void ChatClient::BindFriends(users_cb_t friends_cb)
{
    this->friends_cb = friends_cb;
}
void ChatClient::UnbindFriends()
{
    this->friends_cb = [](auto){};
}
void ChatClient::BindChatMsg(chat_msg_cb_t chat_msg_cb)
{
    this->chat_msg_cb = chat_msg_cb;
}
void ChatClient::UnbindChatMsg()
{
    this->chat_msg_cb = [](auto,auto){};
}
void ChatClient::BindChatMsgs(chat_msgs_cb_t chat_msgs_cb)
{
    this->chat_msgs_cb = chat_msgs_cb;
}
void ChatClient::UnbindChatMsgs()
{
    this->chat_msgs_cb = [](auto,auto){};
}

void ChatClient::RecvLoop()
{
    loop_thread = thread([=]() {
        while(true) {
            BinMsg msg = client->Recv();
            string err;
            Json data = Json::parse(msg->data(), err);

            LOG_DEBUG << msg->data();

            if (data["command"] == COMMAND_ERROR) {
                err_cb(data["msg"].string_value());
                continue;
            }
            if (data["command"] == COMMAND_MSG) {
                msg_cb(data["msg"].string_value());
                continue;
            }
            if (data["command"] == COMMAND_USERS) {
                vector<user_t> users;
                auto users_data = data["users"].array_items();
                for(int i = 0; i < users_data.size(); i ++) {
                    users.push_back((user_t){users_data[i]["username"].string_value(), users_data[i]["online"].bool_value(), users_data[i]["unread"].int_value()});
                }
                users_cb(users);
                continue;
            }
            if (data["command"] == COMMAND_FRIENDS) {
                vector<user_t> friends;
                auto friends_data = data["friends"].array_items();
                for(int i = 0; i < friends_data.size(); i ++) {
                    friends.push_back((user_t){friends_data[i]["username"].string_value(), friends_data[i]["online"].bool_value(), friends_data[i]["unread"].int_value()});
                }
                friends_cb(friends);
                continue;
            }
            if (data["command"] == COMMAND_MSGS) {
                vector<message_t> messages;
                auto messages_data = data["msgs"].array_items();
                for(int i = 0; i < messages_data.size(); i ++) {
                    messages.push_back((message_t){
                        messages_data[i]["id"].int_value(),
                        messages_data[i]["sender"].string_value(),
                        messages_data[i]["msg"].string_value(),
                        messages_data[i]["datetime"].string_value(),
                    });
                }
                chat_msgs_cb(data["username"].string_value(), messages);
                continue;
            }
            if (data["command"] == COMMAND_NEW_MSG) {
                message_t msg = (message_t) {
                    data["id"].int_value(),
                    data["sender"].string_value(),
                    data["msg"].string_value(),
                    data["datetime"].string_value(),
                };
                chat_msg_cb(data["username"].string_value(), msg);
                continue;
            }
            if (data["command"] == COMMAND_SEND_FILE_REQ_S2C) {
                file_cb(data["username"].string_value(), data["filename"].string_value(), [=](bool accept) {
                    task_que.Push([=]() {
                        Json::object obj = Json::object {
                            {"command", COMMAND_SEND_FILE_RES_C2S},
                            {"username", data["username"]},
                            {"accept", accept}
                        };
                        file_client->Send(Json(obj).dump());
                    });
                });
                continue;
            }

            LOG_ERROR << "Unknown msg : " << data.dump();
        }
    });
}

void ChatClient::Flash()
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_FLASH},
        };
        client->Send(data.dump());
    });
}

void ChatClient::AddFriend(string username)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_ADD_FRIEND},
            {"username", username}
        };
        client->Send(data.dump());
    });
}

void ChatClient::QueryMsgs(string username)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_QUERY_MSGS},
            {"username", username}
        };
        client->Send(data.dump());
    });
}
void ChatClient::SendMsg(string username, string msg)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_SEND_MSG},
            {"username", username},
            {"msg", msg}
        };
        client->Send(data.dump());
    });
}
void ChatClient::AcceptMsg(string username, int msg_id)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_ACCEPT_MSG},
            {"username", username},
            {"id", msg_id}
        };
        client->Send(data.dump());
    });
}
void ChatClient::RejectMsg(string username, int msg_id)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_REJECT_MSG},
            {"username", username},
            {"id", msg_id}
        };
        client->Send(data.dump());
    });
}

void ChatClient::Connect(string address, next_cb_t next_cb)
{
    task_que.Push([=]() {
        client = make_shared<TCPClient>(address, SERVER_PORT);
        if (!client->Connect()) {
            err_cb("连接服务器失败");
            next_cb(false);
            return;
        }
        next_cb(true);
        file_client = make_shared<TCPClient>(address, FILE_SERVER_PORT);
        file_client->Connect();
    });
}

void ChatClient::Close()
{
    task_que.Push([=]() {
        client->Close();
    });
}

void ChatClient::Login(string username, string password, next_cb_t next_cb)
{
    task_que.Push([=] {
        Json::object obj = Json::object {
            {"command", COMMAND_LOGIN},
            {"username", username},
            {"password", password}
        };
        client->Send(Json(obj).dump());
        Json res = waitForCommand(COMMAND_LOGIN_RES, client);
        if (res["success"].bool_value()) {
            logined_username = username;
            logined_password = password;
            obj["command"] = COMMAND_FILED_LOGIN;
            file_client->Send(Json(obj).dump());
            next_cb(true);
        } else {
            err_cb(res["msg"].string_value());
            next_cb(false);
        }
    });
}

void ChatClient::Register(string username, string password, next_cb_t next_cb)
{
    task_que.Push([=] {
        Json data = Json::object {
            {"command", COMMAND_REGISTER},
            {"username", username},
            {"password", password}
        };
        client->Send(data.dump());
        Json res = waitForCommand(COMMAND_REGISTER_RES, client);
        if (res["success"].bool_value()) {
            next_cb(true);
        } else {
            err_cb(res["msg"].string_value());
            next_cb(false);
        }
    });
}

void ChatClient::SendFile(string username, string filename, next_cb_t next_cb)
{
    task_que.Push([=]() {
        Json data = Json::object {
            {"command", COMMAND_SEND_FILE_REQ_C2S},
            {"username", username},
            {"filename", filename}
        };
        file_client->Send(data.dump());
        Json res = waitForCommand(COMMAND_SEND_FILE_RES_S2C, file_client);
        next_cb(res["accept"].bool_value());
        if (!res["accept"].bool_value()) {
            err_cb(res["msg"].string_value());
        }
    });
}

void ChatClient::BindFile(file_cb_t file_cb)
{
    this->file_cb = file_cb;
}

void ChatClient::StartSendFile(string send_filename, FileClient::progress_cb_t progress_cb, FileClient::finished_cb_t finished_cb)
{
    Json res = waitForCommand(COMMAND_SEND_FILE_PORT, file_client);
    int port = res["port"].int_value();
    com_file_client->SendFile(port, send_filename, progress_cb, finished_cb);
}
void ChatClient::StartRecvFile(string save_filename, FileClient::progress_cb_t progress_cb, FileClient::finished_cb_t finished_cb)
{
    Json res = waitForCommand(COMMAND_SEND_FILE_PORT, file_client);
    int port = res["port"].int_value();
    com_file_client->RecvFile(port, save_filename, progress_cb, finished_cb);
}

string ChatClient::LoginedUsername()
{
    return logined_username;
}

Json ChatClient::waitForCommand(string command_type, TCPClient::ptr c)
{
    while(true)
    {
        BinMsg msg = c->Recv();
        string err;
        Json data = Json::parse(msg->data(), err);
        if (data["command"] == command_type) return data;
    }
}