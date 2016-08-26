/*******************************************************************************
*
* Copyright 2016 Stefan Majewsky <majewsky@gmx.net>
*
* This program is free software: you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published
* by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
*******************************************************************************/

#include "xmpp-bridge.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <strophe.h>

#ifdef RELEASE
#   define MY_LOG_LEVEL XMPP_LEVEL_INFO
#else
#   define MY_LOG_LEVEL XMPP_LEVEL_DEBUG
#endif

struct Config {
    const char* jid;
    const char* password;
    const char* peer_jid;
    xmpp_ctx_t* ctx;
    bool        connected;
};

void send_presence(xmpp_conn_t* conn, const struct Config* cfg) {
    //send <presence/> to appear online to contacts
    xmpp_stanza_t* pres = xmpp_stanza_new(cfg->ctx);
    xmpp_stanza_set_name(pres, "presence");
    xmpp_send(conn, pres);
    xmpp_stanza_release(pres);
}

int message_handler(xmpp_conn_t* const conn, xmpp_stanza_t* const stanza, void* const userdata) {
    (void) conn;
    //userdata contains a Config struct
    struct Config* cfg = (struct Config*) userdata;

    //check JID of sender
    char* other_jid = xmpp_stanza_get_attribute(stanza, "from");
    if (!match_jid(other_jid, cfg->peer_jid)) {
        return 1;
    }

    //check if there is a body
    xmpp_stanza_t* body = xmpp_stanza_get_child_by_name(stanza, "body");
    if (body == NULL) {
        return 1;
    }
    char* message = xmpp_stanza_get_text(body);
    if (message == NULL) {
        return 1;
    }

    //print message text
    //TODO: check write errors
    fputs(message, stdout);
    if (message[strlen(message) - 1] != '\n') {
        fputc('\n', stdout);
    }
    fflush(stdout);
    xmpp_free(cfg->ctx, message);

    return 1;
}

void conn_handler(xmpp_conn_t* const conn, const xmpp_conn_event_t event, const int error, xmpp_stream_error_t* const stream_error, void* const userdata) {
    //TODO handle these arguments (if necessary)
    (void) error;
    (void) stream_error;
    //userdata contains a Config struct
    struct Config* cfg = (struct Config*) userdata;

    if (event == XMPP_CONN_CONNECT) {
        xmpp_handler_add(conn, message_handler, NULL, "message", "chat", cfg);
        send_presence(conn, cfg);
        cfg->connected = true;
    }
    else {
        //disconnected or a failure occurred
        //TODO: on disconnect, check stdin for EOF and reopen if not yet exhausted
        cfg->connected = false;
    }
}

#define STDIN 0

int main(int argc, char** argv) {
    //get arguments (TODO: validate JIDs)
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <own-jid> <pass> <peer-jid>\n", argv[0]);
        exit(1);
    }
    struct Config cfg = {
        .jid       = strdup(argv[1]),
        .password  = strdup(argv[2]),
        .peer_jid  = strdup(argv[3]),
        .ctx       = NULL,
        .connected = false,
    };
    bool valid = true;
    if (!validate_jid(cfg.jid)) {
        fprintf(stderr, "FATAL: '%s' is not a valid JID\n", cfg.jid);
        valid = false;
    }
    if (!validate_jid(cfg.peer_jid)) {
        fprintf(stderr, "FATAL: '%s' is not a valid JID\n", cfg.peer_jid);
        valid = false;
    }
    if (!valid) {
        return 1;
    }

    //initialize libstrophe context
    xmpp_initialize();
    xmpp_log_t* log = xmpp_get_default_logger(MY_LOG_LEVEL);
    cfg.ctx = xmpp_ctx_new(NULL, log);

    //initialize connection object
    xmpp_conn_t* conn = xmpp_conn_new(cfg.ctx);
#ifdef XMPP_CONN_FLAG_MANDATORY_TLS
    xmpp_conn_set_flags(conn, XMPP_CONN_FLAG_MANDATORY_TLS); //there's just no excuse not to do TLS
#endif
    xmpp_conn_set_jid(conn, cfg.jid);
    xmpp_conn_set_pass(conn, cfg.password);

    //enter the first event loop which creates the connection and then returns
    //(see xmpp_stop() call in conn_handler())
    xmpp_connect_client(conn, NULL, 0, conn_handler, &cfg);
    while (!cfg.connected) {
        xmpp_run_once(cfg.ctx, 100);
    }

    //enter the second event loop which sends and receives messages
    struct ReadBuffer buf;
    readbuffer_init(&buf, STDIN);
    bool stay_in_loop = true;

    while (stay_in_loop && cfg.connected) {
        const bool success = readbuffer_read(&buf, 100000); //timeout = 100 ms
        if (!success) {
            //error -> shutdown
            xmpp_disconnect(conn);
            stay_in_loop = false;
        } else {
            //check if a full line was received
            while (true) {
                char* str = readbuffer_getline(&buf);
                if (str == NULL) {
                    if (buf.eof) {
                        //EOF has been reached - commence normal shutdown
                        xmpp_disconnect(conn);
                        stay_in_loop = false;
                    }
                    //no more lines available at least until the next read()
                    break;
                } else {
                    //send message
                    xmpp_stanza_t* reply = xmpp_stanza_new(cfg.ctx);
                    xmpp_stanza_set_name(reply, "message");
                    xmpp_stanza_set_type(reply, "chat");
                    xmpp_stanza_set_attribute(reply, "from", cfg.jid);
                    xmpp_stanza_set_attribute(reply, "to", cfg.peer_jid);

                    xmpp_stanza_t* body = xmpp_stanza_new(cfg.ctx);
                    xmpp_stanza_set_name(body, "body");

                    xmpp_stanza_t* text = xmpp_stanza_new(cfg.ctx);
                    xmpp_stanza_set_text(text, str);

                    xmpp_stanza_add_child(body, text);
                    xmpp_stanza_add_child(reply, body);

                    xmpp_send(conn, reply);
                    xmpp_stanza_release(reply);
                    free(str);
                }
            }
        }

        //send queued messages and watch for incoming messages, but don't block
        xmpp_run_once(cfg.ctx, 0);
    }

    //wait for disconnect to finish
    while (cfg.connected) {
        xmpp_run_once(cfg.ctx, 100);
    }

    //free resources
    xmpp_conn_release(conn);
    xmpp_ctx_free(cfg.ctx);
    xmpp_shutdown();

    //TODO: which of the xmpp_ functions can throw errors?
    return 0;
}