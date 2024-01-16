//
// Created by just do it on 2024/1/12.
//
#include <assert.h>
#include "mqtt_db.h"
#include "mqtt/mqtt_acl.h"
#include "store/mqtt_msg_store.h"
#include "tlog.h"

void store_messages_to_mongodb(mongoc_client_t* mongo_client, const char* mqtt_client_id, sending_packet_t* packets,
                               int n_packets)
{
    const bson_t** documents = malloc(sizeof(bson_t*) * n_packets);
    sending_packet_t* p = packets;
    for(int i = 0; i < n_packets; i++)
    {
        tmq_publish_pkt* publish_pkt = p->packet.packet_ptr;
        documents[i] = BCON_NEW("client_id", BCON_UTF8(mqtt_client_id),
                                "timestamp", BCON_INT64(p->store_timestamp),
                                "message", "{",
                                "packet_id", BCON_INT32(p->packet_id),
                                "flags", BCON_INT32(publish_pkt->flags),
                                "topic", BCON_UTF8(publish_pkt->topic),
                                "payload", BCON_UTF8(publish_pkt->payload),
                                "}");
        sending_packet_t* next = p->next;
        tmq_any_pkt_cleanup(&p->packet);
        free(p);
        p = next;
    }
    bson_error_t error;
    mongoc_collection_t* collection = mongoc_client_get_collection(mongo_client, "tinyMQTT_message_db", "messages");
    if(!mongoc_collection_insert_many(collection, documents, n_packets, NULL, NULL, &error))
        tlog_error("error occurred when storing messages to mongodb: %s", error.message);
    mongoc_collection_destroy(collection);
}

int fetch_messages_from_mongodb(mongoc_client_t* mongo_client, const char* mqtt_client_id, int limit,
                                sending_packet_t** result_head, sending_packet_t** result_tail)
{
    bson_t* filter = BCON_NEW("client_id", BCON_UTF8(mqtt_client_id));
    bson_t* opts = BCON_NEW("limit", BCON_INT64(limit), "sort", "{", "timestamp", BCON_INT32(1), "}");
    mongoc_collection_t* collection = mongoc_client_get_collection(mongo_client, "tinyMQTT_message_db", "messages");
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, filter, opts, NULL);
    bson_destroy(filter);
    bson_destroy(opts);

    bson_t delete_filter = BSON_INITIALIZER;
    bson_t delete_object_ids;
    const bson_t* doc;
    int cnt = 0;
    sending_packet_t* head = NULL;
    sending_packet_t** p = &head;
    bson_append_array_begin(&delete_filter, "$in", strlen("$in"), &delete_object_ids);
    while (mongoc_cursor_next(cursor, &doc))
    {
        bson_iter_t iter;
        if(bson_iter_init(&iter, doc))
        {
            *p = malloc(sizeof(sending_packet_t));
            bzero(*p, sizeof(sending_packet_t));
            (*p)->packet.packet_type = MQTT_PUBLISH;
            tmq_publish_pkt* message = malloc(sizeof(tmq_publish_pkt));

            bson_iter_next(&iter);
            const bson_oid_t* oid = bson_iter_oid(&iter);
            char str[16];
            const char* key;
            bson_uint32_to_string(cnt++, &key, str, sizeof(str));
            bson_append_oid(&delete_object_ids, key, strlen(key), oid);
            bson_iter_next(&iter);
            bson_iter_next(&iter);
            bson_iter_next(&iter);
            bson_iter_t msg_iter;
            bson_iter_recurse(&iter, &msg_iter);
            bson_iter_next(&msg_iter);
            int32_t packet_id = bson_iter_int32(&msg_iter);
            (*p)->packet_id = packet_id;
            message->packet_id = packet_id;
            bson_iter_next(&msg_iter);
            message->flags = bson_iter_int32(&msg_iter);
            bson_iter_next(&msg_iter);
            message->topic = tmq_str_new(bson_iter_utf8(&msg_iter, 0));
            bson_iter_next(&msg_iter);
            message->payload = tmq_str_new(bson_iter_utf8(&msg_iter, 0));
            (*p)->packet.packet_ptr = message;
            p = &(*p)->next;
        }
    }
    mongoc_cursor_destroy (cursor);
    bson_append_array_end(&delete_filter, &delete_object_ids);
    bson_t* delete_query = BCON_NEW("_id", BCON_DOCUMENT(&delete_filter));
    mongoc_collection_delete_many(collection, delete_query, NULL, NULL, NULL);
    mongoc_collection_destroy(collection);
    bson_destroy(delete_query);
    *result_head = head;
    *result_tail = head ? (sending_packet_t*)p : NULL;
    return cnt;
}

void load_acl_from_mysql(MYSQL* mysql_conn, tmq_acl_t* acl)
{
    static const char* acl_query = "SELECT * FROM tinymqtt_acl_table";
    if(mysql_real_query(mysql_conn, acl_query, strlen(acl_query)) != 0)
    {
        tlog_error("mysql_real_query() error");
        return;
    }
    MYSQL_RES* res = mysql_store_result(mysql_conn);
    if(!res) return;
    MYSQL_ROW row = NULL;
    while((row = mysql_fetch_row(res)) != NULL)
    {
        unsigned int num_fields = mysql_num_fields(res);
        unsigned long* lengths = mysql_fetch_lengths(res);
        assert(num_fields == 7);
        tmq_permission_e permission = atoi(row[1]);
        tmq_access_e access = atoi(row[5]);
        if(lengths[2] > 0)
        {
            tlog_info("load acl rule: %s ip [%s] %s %s",
                      permission_str[permission], row[2], access_str[access], row[6]);
            tmq_acl_add_rule(acl, row[6], acl_ip_rule_new(permission, row[2], access));

        }
        else if(lengths[3] > 0)
        {
            tlog_info("load acl rule: %s user [%s] %s %s",
                      permission_str[permission], row[3], access_str[access], row[6]);
            tmq_acl_add_rule(acl, row[6], acl_username_rule_new(permission, row[3], access));
        }
        else if(lengths[4] > 0)
        {
            tlog_info("load acl rule: %s client_id [%s] %s %s",
                      permission_str[permission], row[4], access_str[access], row[6]);
            tmq_acl_add_rule(acl, row[6], acl_client_id_rule_new(permission, row[4], access));
        }
        else
        {
            tlog_info("load acl rule: %s all %s %s",
                      permission_str[permission], access_str[access], row[6]);
            tmq_acl_add_rule_for_all(acl, row[6], acl_all_rule_new(permission,  access));
        }
    }
    mysql_free_result(res);
}