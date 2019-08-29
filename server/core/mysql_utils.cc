/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */


/**
 * @file mysql_utils.c  - Binary MySQL data processing utilities
 *
 * This file contains functions that are used when processing binary format
 * information. The MySQL protocol uses the binary format in result sets and
 * row based replication.
 */

#include <maxscale/mysql_utils.hh>

#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <errmsg.h>

#include <maxbase/atomic.hh>
#include <maxbase/format.hh>
#include <maxsql/mariadb.hh>
#include <maxbase/alloc.h>
#include <maxscale/config.hh>

MYSQL* mxs_mysql_real_connect(MYSQL* con, SERVER* server, const char* user, const char* passwd)
{
    auto ssl = server->ssl().config();
    bool have_ssl = ssl && !ssl->empty();
    if (have_ssl)
    {
        // If an option is empty, a null-pointer should be given to mysql_ssl_set.
        const char* ssl_key = ssl->key.empty() ? nullptr : ssl->key.c_str();
        const char* ssl_cert = ssl->cert.empty() ? nullptr : ssl->cert.c_str();
        const char* ssl_ca = ssl->ca.empty() ? nullptr : ssl->ca.c_str();
        mysql_ssl_set(con, ssl_key, ssl_cert, ssl_ca, NULL, NULL);
    }

    char yes = 1;
    mysql_optionsv(con, MYSQL_OPT_RECONNECT, &yes);
    mysql_optionsv(con, MYSQL_INIT_COMMAND, "SET SQL_MODE=''");

    MXS_CONFIG* config = config_get_global_options();

    if (config->local_address)
    {
        if (mysql_optionsv(con, MYSQL_OPT_BIND, config->local_address) != 0)
        {
            MXS_ERROR("'local_address' specified in configuration file, but could not "
                      "configure MYSQL handle. MaxScale will try to connect using default "
                      "address.");
        }
    }

    MYSQL* mysql = nullptr;

    if (server->address[0] == '/')
    {
        mysql = mysql_real_connect(con, nullptr, user, passwd, nullptr, 0, server->address, 0);
    }
    else
    {
        mysql = mysql_real_connect(con, server->address, user, passwd, NULL, server->port, NULL, 0);
        auto extra_port = mxb::atomic::load(&server->extra_port, mxb::atomic::RELAXED);

        if (!mysql && extra_port > 0)
        {
            mysql = mysql_real_connect(con, server->address, user, passwd, NULL, extra_port, NULL, 0);
            MXS_WARNING("Could not connect with normal port to server '%s', using extra_port",
                        server->name());
        }
    }

    if (mysql)
    {
        /** Copy the server charset */
        MY_CHARSET_INFO cs_info;
        mysql_get_character_set_info(mysql, &cs_info);
        server->charset = cs_info.number;

        if (have_ssl && mysql_get_ssl_cipher(con) == NULL)
        {
            if (server->warn_ssl_not_enabled)
            {
                server->warn_ssl_not_enabled = false;
                MXS_ERROR("An encrypted connection to '%s' could not be created, "
                          "ensure that TLS is enabled on the target server.",
                          server->name());
            }
            // Don't close the connection as it is closed elsewhere, just set to NULL
            mysql = NULL;
        }
    }

    return mysql;
}

int mxs_mysql_query(MYSQL* conn, const char* query)
{
    MXS_CONFIG* cnf = config_get_global_options();
    return maxsql::mysql_query_ex(conn, query, cnf->query_retries, cnf->query_retry_timeout);
}

const char* mxs_mysql_get_value(MYSQL_RES* result, MYSQL_ROW row, const char* key)
{
    MYSQL_FIELD* f = mysql_fetch_fields(result);
    int nfields = mysql_num_fields(result);

    for (int i = 0; i < nfields; i++)
    {
        if (strcasecmp(f[i].name, key) == 0)
        {
            return row[i];
        }
    }

    return NULL;
}

bool mxs_mysql_trim_quotes(char* s)
{
    bool dequoted = true;

    char* i = s;
    char* end = s + strlen(s);

    // Remove space from the beginning
    while (*i && isspace(*i))
    {
        ++i;
    }

    if (*i)
    {
        // Remove space from the end
        while (isspace(*(end - 1)))
        {
            *(end - 1) = 0;
            --end;
        }

        mxb_assert(end > i);

        char quote;

        switch (*i)
        {
        case '\'':
        case '"':
        case '`':
            quote = *i;
            ++i;
            break;

        default:
            quote = 0;
        }

        if (quote)
        {
            --end;

            if (*end == quote)
            {
                *end = 0;

                memmove(s, i, end - i + 1);
            }
            else
            {
                dequoted = false;
            }
        }
        else if (i != s)
        {
            memmove(s, i, end - i + 1);
        }
    }
    else
    {
        *s = 0;
    }

    return dequoted;
}


mxs_mysql_name_kind_t mxs_mysql_name_to_pcre(char* pcre,
                                             const char* mysql,
                                             mxs_pcre_quote_approach_t approach)
{
    mxs_mysql_name_kind_t rv = MXS_MYSQL_NAME_WITHOUT_WILDCARD;

    while (*mysql)
    {
        switch (*mysql)
        {
        case '%':
            if (approach == MXS_PCRE_QUOTE_WILDCARD)
            {
                *pcre = '.';
                pcre++;
                *pcre = '*';
            }
            rv = MXS_MYSQL_NAME_WITH_WILDCARD;
            break;

        case '\'':
        case '^':
        case '.':
        case '$':
        case '|':
        case '(':
        case ')':
        case '[':
        case ']':
        case '*':
        case '+':
        case '?':
        case '{':
        case '}':
            *pcre++ = '\\';

        // Flowthrough
        default:
            *pcre = *mysql;
        }

        ++pcre;
        ++mysql;
    }

    *pcre = 0;

    return rv;
}

void mxs_mysql_update_server_version(SERVER* dest, MYSQL* source)
{
    // This function should only be called for a live connection.
    const char* version_string = mysql_get_server_info(source);
    unsigned long version_num = mysql_get_server_version(source);
    mxb_assert(version_string != NULL && version_num != 0);
    dest->set_version(version_num, version_string);
}

namespace maxscale
{

std::unique_ptr<mxq::QueryResult> execute_query(MYSQL* conn, const std::string& query,
                                                std::string* errmsg_out, unsigned int* errno_out)
{
    using mxq::QueryResult;
    std::unique_ptr<QueryResult> rval;
    MYSQL_RES* result = NULL;
    if (mxs_mysql_query(conn, query.c_str()) == 0 && (result = mysql_store_result(conn)) != NULL)
    {
        rval = std::unique_ptr<QueryResult>(new QueryResult(result));
    }
    else
    {
        if (errmsg_out)
        {
            *errmsg_out = mxb::string_printf("Query '%s' failed: '%s'.", query.c_str(), mysql_error(conn));
        }

        if (errno_out)
        {
            *errno_out = mysql_errno(conn);
        }
    }

    return rval;
}
}

#if defined (SS_DEBUG)
/**
 * Return decoded MySQL response packet.
 *
 * Intended to be used when debugging with a GDB-based debugger.
 * For instance, if GDB has been stopped by a breakpoint in
 * clientReply() you can use this function for investigating
 * what the response packet contains:
 *
 * (gdb) p dbg_decode_response(writebuf)
 * $30 = 0x7ffff0d40d54 "Packet no: 1, Payload len: 44, Command : ERR,
 * Code: 1146, Message : Table 'test.blahasdf' doesn't exist"
 *
 * @param pPacket  A MySQL response packet.
 *
 * @return The packet decoded into a descriptive string.
 */
const char* dbg_decode_response(GWBUF* pPacket)
{
    thread_local std::string rv;

    std::stringstream ss;

    mxs::Buffer b(pPacket);
    int nRemaining = b.length();
    auto it = b.begin();

    while (nRemaining > MYSQL_HEADER_LEN + 1)
    {
        if (!ss.str().empty())
        {
            ss << "\n";
        }

        uint8_t header[MYSQL_HEADER_LEN + 1];

        auto start = it;
        auto end = std::next(it, sizeof(header));
        std::copy(it, end, header);
        it = end;

        uint32_t payload_len = MYSQL_GET_PAYLOAD_LEN(header);
        uint32_t packet_len = MYSQL_HEADER_LEN + payload_len;
        uint32_t packet_no = MYSQL_GET_PACKET_NO(header);
        uint32_t command = MYSQL_GET_COMMAND(header);

        ss << "Packet no: " << packet_no << ", Payload len: " << payload_len;

        switch (command)
        {
        case 0x00:
            ss << ", Command : OK";
            break;

        case 0xff:
            {
                ss << ", Command : ERR";

                uint8_t error[payload_len];
                error[0] = *it;

                end = std::next(it, sizeof(error) - 1); // -1 due to the 1 in 'header' above.
                std::copy(it, end, error + 1);

                uint32_t error_code = gw_mysql_get_byte2(&error[1]);

                ss << ", Code: " << error_code;

                const int message_index = 1 + 2 + 1 + 5;
                uint8_t* pMessage = &error[message_index];
                int message_len = payload_len - message_index;

                ss << ", Message : ";

                ss.write(reinterpret_cast<const char*>(pMessage), message_len);
            }
            break;

        case 0xfb:
            ss << ", Command : GET_MORE_CLIENT_DATA";
            break;

        default:
            ss << ", Command : Result Set";
        }

        it = std::next(start, MYSQL_HEADER_LEN + payload_len);

        nRemaining -= MYSQL_HEADER_LEN;
        nRemaining -= payload_len;
    }

    b.release();

    rv = ss.str();

    return rv.c_str();
}
#endif
