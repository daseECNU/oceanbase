/**
 * (C) 2007-2010 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * ob_log_generator.h
 *
 * Authors:
 *   yuanqi.xhf <yuanqi.xhf@taobao.com>
 *
 */

#ifndef __OB_COMMON_OB_LOG_GENERATOR_H__
#define __OB_COMMON_OB_LOG_GENERATOR_H__

#include "ob_log_entry.h"
#include "ob_log_cursor.h"

using namespace oceanbase::common;
namespace oceanbase
{
  namespace common
  {
    struct DebugLog
    {
      enum { MAGIC = 0xde6a9de6a901 };
      DebugLog(): server_(), ctime_(0), last_ctime_(0) {}
      ~DebugLog() {}
      ObServer server_;
      int64_t ctime_;
      int64_t last_ctime_;
      int advance();
      int64_t to_string(char* buf, const int64_t len) const;
      int serialize(char* buf, int64_t limit, int64_t& pos) const;
      int deserialize(const char* buf, int64_t limit, int64_t& pos);
    };
    class ObLogGenerator
    {
      public:
        static const int64_t LOG_FILE_ALIGN_SIZE = 1<<OB_DIRECT_IO_ALIGN_BITS;
        static const int64_t LOG_FILE_ALIGN_MASK = LOG_FILE_ALIGN_SIZE - 1;
        static const int64_t LOG_BUF_RESERVED_SIZE = 3 * LOG_FILE_ALIGN_SIZE; // nop + switch_log + eof
      public:
        ObLogGenerator();
        ~ObLogGenerator();
        int init(int64_t log_buf_size, int64_t log_file_max_size, const ObServer* id=NULL);
        int reset();
        bool is_log_start() const;
        int start_log(const ObLogCursor& start_cursor);
        bool check_log_size(const int64_t size) const;
        int update_cursor(const ObLogCursor& log_cursor); // 备机写日志时也会更新log_generator的cursor
        int fill_batch(const char* buf, int64_t len);
        int write_log(const LogCommand cmd, const char* log_data, const int64_t data_len);
        //add chujiajia [log synchronization][multi_cluster] 20160328:b
        /**
         * @brief write log using the input parameters
         * @param [in] cmd log command
         * @param [in] log_data log content
         * @param [in] data_len the length of the log_data
         * @param [in] max_cmt_id max commited log id
         * @return OB_SUCCESS if success
         */
        int write_log(const LogCommand cmd, const char* log_data, const int64_t data_len, const int64_t max_cmt_id);
        //add:e
        template<typename T>
        int write_log(const LogCommand cmd, T& data);
        //add chujiajia [log synchronization][multi_cluster] 20160603:b
        /**
         * @brief write log using the input parameters
         * @param [in] cmd log command
         * @param [in] data log content
         * @param [in] max_cmt_id max commited log id
         * @return OB_SUCCESS if success
         */
        template<typename T>
        int write_log(const LogCommand cmd, T& data, const int64_t max_cmt_id);
        //add:e
        //modify chujiajia [log synchronization][multi_cluster] 20160328:b
        //int get_log(ObLogCursor& start_cursor, ObLogCursor& end_cursor, char*& buf, int64_t& len);
        int get_log(ObLogCursor& start_cursor, ObLogCursor& end_cursor, char*& buf, int64_t& len, const int64_t max_cmt_id);
        //mod:e
        int commit(const ObLogCursor& end_cursor);
        int switch_log(int64_t& new_file_id);
        int check_point(int64_t& cur_log_file_id);
        //modify chujiajia [log synchronization][multi_cluster] 20160328:b
        //int gen_keep_alive();
        int gen_keep_alive(const bool is_ups_nop, const int64_t max_cmt_id);
        //mod:e
        bool is_clear() const;
        int64_t to_string(char* buf, const int64_t len) const;
        static bool is_eof(const char* buf, int64_t len);
      public:
        int get_start_cursor(ObLogCursor& log_cursor) const;
        int get_end_cursor(ObLogCursor& log_cursor) const;
		//add chujiajia [log synchronization][multi_cluster] 20160625:b
        /**
         * @brief set start_cursor_
         * @param [in] log_cursor specific log cursor
         * @return OB_SUCCESS if success
         */
        void set_start_cursor(ObLogCursor& log_cursor);
        /**
         * @brief set end_cursor_
         * @param [in] log_cursor specific log cursor
         * @return OB_SUCCESS if success
         */
        void set_end_cursor(ObLogCursor& log_cursor);
		//add:e
        int dump_for_debug() const;
      protected:
        bool is_inited() const;
        int check_state() const;
        bool has_log() const;
        int do_write_log(const LogCommand cmd, const char* log_data, const int64_t data_len,
                         const int64_t reserved_len);
        //add chujiajia [log synchronization][multi_cluster] 20160328:b
        /**
         * @brief do write log using the input parameters
         * @param [in] cmd log command
         * @param [in] log_data log content
         * @param [in] log_data log content
         * @param [in] max_cmt_id max commited log id
         * @return OB_SUCCESS if success
         */
        int do_write_log(const LogCommand cmd,
                         const char* log_data,
                         const int64_t data_len,
                         const int64_t reserved_len,
                         const int64_t max_cmt_id);
        //add:e
        int check_log_file_size();
        int switch_log();
        int write_nop(const bool force_write=false);
        //add chujiajia [log synchronization][multi_cluster] 20160328:b
        /**
         * @brief write NOP log
         * @param [in] is_ups_nop if it is ups nop log or not
         * @param [in] max_cmt_id max commited log id
         * @param [in] force_write force writed or not
         * @return OB_SUCCESS if success
         */
        int write_nop(const bool is_ups_nop, const int64_t max_cmt_id, const bool force_write=false);
        //modify:e
        int append_eof();
      public:
        static char eof_flag_buf_[LOG_FILE_ALIGN_SIZE] __attribute__ ((aligned(DIO_ALIGN_SIZE)));
      private:
        bool is_frozen_;
        int64_t log_file_max_size_;
        ObLogCursor start_cursor_;
        ObLogCursor end_cursor_;
        char* log_buf_;
        int64_t log_buf_len_;
        int64_t pos_;
        DebugLog debug_log_;
        char empty_log_[LOG_FILE_ALIGN_SIZE * 2];
        char nop_log_[LOG_FILE_ALIGN_SIZE * 2];
    };

    template<typename T>
    int generate_log(char* buf, const int64_t len, int64_t& pos, ObLogCursor& cursor, const LogCommand cmd,
                 const T& data)
    {
      int err = OB_SUCCESS;
      ObLogEntry entry;
      int64_t new_pos = pos;
      int64_t data_pos = pos + entry.get_serialize_size();
      int64_t end_pos = data_pos;
      if (NULL == buf || 0 >= len || pos > len || !cursor.is_valid())
      {
        err = OB_INVALID_ARGUMENT;
        TBSYS_LOG(ERROR, "generate_log(buf=%p, len=%ld, pos=%ld, cursor=%s)=>%d",
                  buf, len, pos, cursor.to_str(), err);
      }
      else if (OB_SUCCESS != (err = data.serialize(buf, len, end_pos)))
      {
        if (entry.get_serialize_size() + data.get_serialize_size() > len)
        {
          err = OB_LOG_TOO_LARGE;
          TBSYS_LOG(WARN, "log too large(size=%ld, limit=%ld)", data.get_serialize_size(), len);
        }
        else
        {
          err = OB_BUF_NOT_ENOUGH;
        }
      }
      else if (OB_SUCCESS != (err = cursor.next_entry(entry, cmd, buf + data_pos, end_pos - data_pos)))
      {
        TBSYS_LOG(ERROR, "cursor[%s].next_entry()=>%d", cursor.to_str(), err);
      }
      else if (OB_SUCCESS != (err = entry.serialize(buf, new_pos + entry.get_serialize_size(), new_pos)))
      {
        TBSYS_LOG(ERROR, "serialize_log_entry(buf=%p, len=%ld, entry[id=%ld], data_len=%ld)=>%d",
                  buf, len, entry.seq_, end_pos - data_pos, err);
      }
      else if (OB_SUCCESS != (err = cursor.advance(entry)))
      {
        TBSYS_LOG(ERROR, "cursor[id=%ld].advance(entry.id=%ld)=>%d", cursor.log_id_, entry.seq_, err);
      }
      else
      {
        pos = end_pos;
      }
      return err;
    }

    template<typename T>
    int ObLogGenerator::write_log(const LogCommand cmd, T& data)
    {
      int err = OB_SUCCESS;
      if (OB_SUCCESS != (err = check_state()))
      {
        TBSYS_LOG(ERROR, "check_state()=>%d", err);
      }
      else if (is_frozen_)
      {
        err = OB_STATE_NOT_MATCH;
        TBSYS_LOG(ERROR, "log_generator is frozen, cursor=[%s,%s]", to_cstring(start_cursor_), to_cstring(end_cursor_));
      }
      else if (OB_SUCCESS != (err = generate_log(log_buf_, log_buf_len_ - LOG_BUF_RESERVED_SIZE, pos_,
                                                 end_cursor_, cmd, data))
               && OB_BUF_NOT_ENOUGH != err)
      {
        TBSYS_LOG(WARN, "generate_log(pos=%ld)=>%d", pos_, err);
      }
      return err;
    }

    //add chujiajia [log synchronization][multi_cluster] 20160603:b
    template<typename T>
    int generate_log(char* buf,
                     const int64_t len,
                     int64_t& pos,
                     ObLogCursor& cursor,
                     const LogCommand cmd,
                     const T& data,
                     const int64_t max_cmt_id)
    {
      int err = OB_SUCCESS;
      ObLogEntry entry;
      int64_t new_pos = pos;
      int64_t data_pos = pos + entry.get_serialize_size();
      int64_t end_pos = data_pos;
      if (NULL == buf || 0 >= len || pos > len || !cursor.is_valid())
      {
        err = OB_INVALID_ARGUMENT;
        TBSYS_LOG(ERROR, "generate_log(buf=%p, len=%ld, pos=%ld, cursor=%s)=>%d",
                  buf, len, pos, cursor.to_str(), err);
      }
      else if (OB_SUCCESS != (err = data.serialize(buf, len, end_pos)))
      {
        if (entry.get_serialize_size() + data.get_serialize_size() > len)
        {
          err = OB_LOG_TOO_LARGE;
          TBSYS_LOG(WARN, "log too large(size=%ld, limit=%ld)", data.get_serialize_size(), len);
        }
        else
        {
          err = OB_BUF_NOT_ENOUGH;
        }
      }
      else if (OB_SUCCESS != (err = cursor.next_entry(entry, cmd, buf + data_pos, end_pos - data_pos, max_cmt_id)))
      {
        TBSYS_LOG(ERROR, "cursor[%s].next_entry()=>%d", cursor.to_str(), err);
      }
      else if (OB_SUCCESS != (err = entry.serialize(buf, new_pos + entry.get_serialize_size(), new_pos)))
      {
        TBSYS_LOG(ERROR, "serialize_log_entry(buf=%p, len=%ld, entry[id=%ld], data_len=%ld)=>%d",
                  buf, len, entry.seq_, end_pos - data_pos, err);
      }
      else if (OB_SUCCESS != (err = cursor.advance(entry)))
      {
        TBSYS_LOG(ERROR, "cursor[id=%ld].advance(entry.id=%ld)=>%d", cursor.log_id_, entry.seq_, err);
      }
      else
      {
        pos = end_pos;
      }
      return err;
    }
    //add:e

    //add chujiajia [log synchronization][multi_cluster] 20160603:b
    template<typename T>
    int ObLogGenerator::write_log(const LogCommand cmd, T& data, const int64_t max_cmt_id)
    {
      int err = OB_SUCCESS;
      if (OB_SUCCESS != (err = check_state()))
      {
        TBSYS_LOG(ERROR, "check_state()=>%d", err);
      }
      else if (is_frozen_)
      {
        err = OB_STATE_NOT_MATCH;
        TBSYS_LOG(ERROR, "log_generator is frozen, cursor=[%s,%s]", to_cstring(start_cursor_), to_cstring(end_cursor_));
      }
      else if (OB_SUCCESS != (err = generate_log(log_buf_, log_buf_len_ - LOG_BUF_RESERVED_SIZE, pos_,
                                                 end_cursor_, cmd, data, max_cmt_id))
               && OB_BUF_NOT_ENOUGH != err)
      {
        TBSYS_LOG(WARN, "generate_log(pos=%ld)=>%d", pos_, err);
      }
      return err;
    }
    //add:e

  } // end namespace common
} // end namespace oceanbase

#endif /* __OB_COMMON_OB_LOG_GENERATOR_H__ */
