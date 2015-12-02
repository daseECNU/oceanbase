/*
 * ob_index_handle_pool.cpp
 *
 *  Created on: 2015年12月2日
 *      Author: longfei
 *  longfei1lantern@gmail.com
 */

#include "ob_index_handle_pool.h"
#include "ob_chunk_server_main.h"
#include "common/ob_read_common_data.h"
#include "ob_tablet_image.h"
#include "common/utility.h"
#include "sstable/ob_disk_path.h"
#include "common/ob_trace_log.h"
#include "ob_tablet_manager.h"
#include "common/ob_atomic.h"
#include "common/file_directory_utils.h"
#include "ob_index_handler.h"
#include "common/hash/ob_hashmap.h"
#include "common/ob_scanner.h"
//add zhuyanchao [secondary index static_data_build]20150326
#include "common/ob_tablet_histogram_report_info.h"
#include "common/ob_tablet_histogram.h"
#include "common/ob_mod_define.h"
//add e
namespace oceanbase
{
  namespace chunkserver
  {
    using namespace tbutil;
    using namespace common;
    using namespace sstable;

   /*-----------------------------------------------------------------------------
    *  Thread for index building
    *-----------------------------------------------------------------------------*/

  ObIndexHandlePool::ObIndexHandlePool() : inited_(false),thread_num_(0),process_idx_tid_(OB_INVALID_ID),schedule_idx_tid_(OB_INVALID_ID),tablet_index_(0),
                                     range_index_(0),hist_width_(0), tablets_have_got_(0), active_thread_num_(0),min_work_thread_num_(0),
                                     round_start_(ROUND_TRUE),round_end_(TABLET_RELEASE),which_stage_(common::ConIdxStage::STAGE_INIT),tablet_manager_(NULL),total_work_last_end_time_(0)
    {
      static_index_report_infolist = OB_NEW(ObTabletHistogramReportInfoList,ObModIds::OB_TABLET_HISTOGRAM_REPORT);
    }

    int ObIndexHandlePool::init(ObTabletManager *manager)
    {
      int ret = OB_SUCCESS;
      ObChunkServer& chunk_server = ObChunkServerMain::get_instance()->get_chunk_server();
      if(NULL == manager)
      {
        TBSYS_LOG(ERROR, "initialize index worker failed,null pointer");
        ret = OB_ERROR;
      }
      else if(!inited_)
      {
        inited_ = true;


        tablet_manager_ = manager;
        schema_mgr_ = chunk_server.get_schema_manager();

        pthread_mutex_init(&mutex_, NULL);
        pthread_mutex_init(&phase_mutex_, NULL);
        pthread_cond_init(&cond_, NULL);

        int64_t max_work_thread_num = chunk_server.get_config().max_merge_thread_num;
        if (max_work_thread_num <= 0 || max_work_thread_num > MAX_WORK_THREAD)
          max_work_thread_num = MAX_WORK_THREAD;

        if(OB_SUCCESS != (ret = set_config_param()))
        {
          TBSYS_LOG(ERROR, "failed to set index work param[%d]",ret);
        }
        else if(OB_SUCCESS != (ret = create_work_thread(max_work_thread_num)))
        {
          TBSYS_LOG(ERROR, "failed to initialize thread for index[%d]",ret);
        }
        else if(OB_SUCCESS != (ret = create_all_index_handlers()))
        {
          TBSYS_LOG(ERROR, "failed to create all index handler[%d]", ret);
        }
        else if(OB_SUCCESS != (ret = black_list_array_.init()))
        {
          TBSYS_LOG(ERROR, "failed to init black list array");
        }
      }
      else
      {
        TBSYS_LOG(WARN,"index worker has been inited!");
      }
      if(OB_SUCCESS != ret && inited_)
      {
        pthread_mutex_destroy(&mutex_);
        pthread_mutex_destroy(&phase_mutex_);
        pthread_cond_destroy(&cond_);
        inited_ = false;
      }
      return ret;
    }

    int ObIndexHandlePool::create_work_thread(const int64_t max_work_thread)
    {
      int ret = OB_SUCCESS;
      setThreadCount(static_cast<int32_t>(max_work_thread));
      active_thread_num_ = max_work_thread;
      thread_num_ = start();

      if(0 >= thread_num_)
      {
        TBSYS_LOG(ERROR,"start thread failed!");
        ret = OB_ERROR;
      }
      else
      {
        if(thread_num_ != max_work_thread)
        {
          TBSYS_LOG(WARN, "failed to start [%ld] threads to build index, there is [%ld] threads", max_work_thread, thread_num_);
        }
        min_work_thread_num_ = thread_num_ / 3;
        if(0 == min_work_thread_num_) min_work_thread_num_ = 1;
        TBSYS_LOG(INFO,"index work thread_num=%ld "
                  "active_thread_num_=%ld, min_merge_thread_num_=%ld",
                  thread_num_, active_thread_num_, min_work_thread_num_);
      }

      return ret;
    }

    int ObIndexHandlePool::create_all_index_handlers()
    {
      int ret = OB_SUCCESS;
      TBSYS_LOG(INFO, "NOW START CREATE INDEX BUILDER");
      if(OB_SUCCESS != (ret = create_index_handlers(handler_, MAX_WORK_THREAD)))
      {
        TBSYS_LOG(ERROR,"failed to create index handlers");
      }
      return ret;
    }

    int ObIndexHandlePool::create_index_handlers(ObIndexHandler **handler, const int64_t size)
    {
      int ret = OB_SUCCESS;
      char* ptr = NULL;
      if(NULL == handler || 0 > size)
      {
        ret = OB_INVALID_ARGUMENT;
        TBSYS_LOG(ERROR, "the pointer of index handler is null");
      }
      else if(NULL == (ptr = reinterpret_cast<char*>(ob_malloc(sizeof(ObIndexHandler)*size, ObModIds::OB_STATIC_INDEX))))
      {
        TBSYS_LOG(WARN, "allocate memory for index handler object error");
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else if(NULL == schema_mgr_)
      {
        TBSYS_LOG(ERROR, "schema_manager pointer is NULL");
        ret = OB_INVALID_ARGUMENT;
      }
      else if(NULL == tablet_manager_)
      {
        //modify liuxiao [secondary index static_index_build.bug_fix.merge_error]20150604
        ret = OB_INVALID_ARGUMENT;
        //modify e
        TBSYS_LOG(ERROR, "tablet_manager pointer is NULL");
      }
      else
      {
        for(int64_t i = 0; i < size; i++)
        {
          //@todo(longfei):to complete ob_index_handle() class
          ObIndexHandler* handlers =  new(ptr + i*sizeof(ObIndexHandler)) ObIndexHandler(this, schema_mgr_, tablet_manager_);
          if(NULL == handlers || OB_SUCCESS != (ret = handlers->init()))
          {
            TBSYS_LOG(WARN, "init index handler error, ret[%d]", ret);
            ret = OB_ERROR;
            break;
          }
          else
          {
            handler[i] = handlers;
          }
        }
      }
      return ret;
    }

    int ObIndexHandlePool::schedule(common::ConIdxStage which_stage)
    {
      int ret = OB_SUCCESS;
      if(!can_launch_next_round())
      {
        TBSYS_LOG(INFO,"cannot launch next round.");
        ret = OB_CS_EAGAIN;
      }
      else if (0 == tablet_manager_->get_serving_data_version())
      {
        TBSYS_LOG(INFO,"empty chunkserver, wait for data");
        ret = OB_CS_EAGAIN;
      }
      else
      {
        ret = start_round(which_stage);
//        TBSYS_LOG(ERROR,"can't understand the stage of cons static index,which_stage = %d",(int)which_stage);
      }

      if(inited_ && OB_SUCCESS == ret && thread_num_ > 0)
      {
        local_work_start_time_ = tbsys::CTimeUtil::getTime();
        round_start_ = ROUND_TRUE;
        round_end_ = ROUND_FALSE;
        pthread_cond_broadcast(&cond_);
      }
      return ret;
    }

    int ObIndexHandlePool:: start_round(common::ConIdxStage which_stage)
    {
      int ret = OB_SUCCESS;

      /*if(phase1_ && !phase2_ && OB_SUCCESS != (ret = prepare_build_index()))
      {
        TBSYS_LOG(ERROR,"start b*uild index round error");
      }
      else if(!phase1_ && phase2_ && OB_SUCCESS != (ret = prepare_build_total_index()))
      {
        TBSYS_LOG(ERROR, "start build index round error");
      }
      */
      if(true)
      {
        //@todo 将process_idx_tid的数据表的tablet所加的附加局部索引sstable删除
        //注意，如果你process_idx_tid == schedule_idx_tid,则不能删除这个局部索引，因为这个时候的状态是处理上次没有完成的索引构建！！
        //既然已经唤醒了一个新的索引构建任务，那么上一次的任务必然是完成的，可能这个CS掉线导致没有同步上任务，那么这些
        //遗留下的局部索引sstable是应该删除的
      }
      if(process_idx_tid_ != schedule_idx_tid_)
      {
        process_idx_tid_ = schedule_idx_tid_;
        //TBSYS_LOG(ERROR, "test::whx process_idx_tid_[%ld]",process_idx_tid_);
      }
      // del longfei
//      if(OB_SUCCESS != (ret = fetch_tablet_info()))
//      {
//        TBSYS_LOG(ERROR,"start build index round error");
//      }
      // del e

      // add longfei [cons static index] 151110:
      if (ConIdxStage::LOCAL_INDEX_STAGE == which_stage)
      {

//        if(OB_SUCCESS != (ret = fetch_tablet_info(true,false)))
//        {
//          TBSYS_LOG(ERROR,"start build index round error");
//        }

        if(OB_SUCCESS != (ret = fetch_tablet_info(LOCAL_INDEX_STAGE)))
        {
          TBSYS_LOG(ERROR,"start build index round error");
        }

      }
      else if (ConIdxStage::GLOBAL_INDEX_STAGE == which_stage)
      {
        if(OB_SUCCESS != (ret = fetch_tablet_info(GLOBAL_INDEX_STAGE)))
        {
          TBSYS_LOG(ERROR,"start build index round error");
        }
      }
      else
      {
        ret = OB_ERROR;
        TBSYS_LOG(ERROR,"can't understand the stage of cons static index,which_stage = %d",(int)which_stage);
      }
      // add e

      return ret;
    }

    // add longfei [cons static index] 151124:b
    int ObIndexHandlePool::fetch_tablet_info(common::ConIdxStage which_stage)
    {
      int ret = OB_SUCCESS;
      if (STAGE_INIT == which_stage)
      {
       TBSYS_LOG(ERROR,"can't understand which stage,which_stage = %d",which_stage);
       ret = OB_ERROR; //@todo(longfei) need to change error type
      }

      if (ret == OB_SUCCESS && LOCAL_INDEX_STAGE == which_stage)
      {
        // get original table's tablet on this cs,and sort them
        const ObSchemaManagerV2* schema_mgr = schema_mgr_->get_schema(process_idx_tid_);
        const ObTableSchema* index_schema = NULL;
        // process_idx_tid is the index table's id && ori_tid is the original table's id
        // by original,i mean the table which index table comes from
        uint64_t ori_tid = OB_INVALID_ID;
        ObVector<ObTablet*> tablet_list;
        if(NULL == schema_mgr)
        {
          TBSYS_LOG(ERROR, "failed in take schema!");
          ret = OB_SCHEMA_ERROR;
        }
        else if(NULL == (index_schema = schema_mgr->get_table_schema(process_idx_tid_)))
        {
          TBSYS_LOG(ERROR, "failed in find schema[%ld]",process_idx_tid_ );
          ret = OB_SCHEMA_ERROR;
        }
        ori_tid = index_schema->get_original_table_id();
        if (OB_SUCCESS
            != (ret =
                tablet_manager_->get_serving_tablet_image().get_serving_image().acquire_tablets_by_table_id(
                    ori_tid, tablet_list)))
        {
          TBSYS_LOG(ERROR, "get tablets error!");
          ret = OB_ERROR;
        }
        if(OB_SUCCESS == ret)
        {
          bool need_index = true;
          for (ObVector<ObTablet*>::iterator it = tablet_list.begin();
              it != tablet_list.end(); ++it)
          {
            /*if(is_need_static_index_tablet(it))
             {
             tablet_array_[tablets_num_]=it;
             tablets_num_++;
             }
             */
            ObTabletLocationList list;
            bool is_handled = false;
            if (OB_SUCCESS == (ret = is_tablet_handle(*it, is_handled)))
            {
              if (!is_handled)
              {
//                if (OB_SUCCESS
//                    != (ret = is_tablet_need_build_static_index(*it, list,
//                        need_index)))
//                {
//                  TBSYS_LOG(ERROR,
//                      "error in is_need_static_index_tablet,ret[%d]", ret);
//                }
                if ((*it)->get_sstable_id_list().count() == ObTablet::MAX_SSTABLE_PER_TABLET)
                {
                  TBSYS_LON(WARN,"This tablet's can't add sstable,sstable'num = %d",ObTablet::MAX_SSTABLE_PER_TABLET);
                  need_index = false;
                }
                else if (need_index)
                {
                  //TBSYS_LOG(INFO,"test::whx add tablet in array");
                  TabletRecord record;
                  record.tablet_ = *it;
                  tablet_array_.push_back(record);
                }
                //add liumz, bugfix: tablet image 20150626:b
                else if (OB_SUCCESS
                    != (ret =
                        tablet_manager_->get_serving_tablet_image().release_tablet(
                            *it)))
                {
                  TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]",
                      ret);
                  //break;//del liumz
                }
                //add:e
              }
              else if (OB_SUCCESS
                  != (ret =
                      tablet_manager_->get_serving_tablet_image().release_tablet(
                          *it)))
              {
                TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                //break;//del liumz
              }
              /*
               else
               {
               //TBSYS_LOG(ERROR,"test::whx release tablet");
               }*/
            }
          }
        }
        if(NULL != schema_mgr)
        {
          if(OB_SUCCESS != (schema_mgr_->release_schema(schema_mgr)))
          {
            TBSYS_LOG(ERROR, "failed to release schema ret[%d]", ret);
          }
        }
      }

      if (ret == OB_SUCCESS && GLOBAL_INDEX_STAGE == which_stage)
      {
        // receive range info from rs, find all the tablet info in this range,put them into range_array_
        // in fact,in the get_ready_for_cons_idx stage,we simply put all the tablet info of this index table into
        // multcs_range_hash,and put the the info of the tablet in this cs into range_hash
        // and then we construct range_arry_
        if (OB_SUCCESS == ret)
        {
          const int64_t timeout = 2000000;
          ObGeneralRpcStub rpc_stub = THE_CHUNK_SERVER.get_rpc_stub();
          ObScanner scanner;
          ObRowkey start_key;
          range_hash_.clear();
          bool need_other_cs = true;
          bool need_release_all_tablet = false;
          if(need_other_cs)
          {
            data_multcs_range_hash_.clear();
          }
          start_key.set_min_row();
          {
            do
            {
              ret = rpc_stub.fetch_tablet_location(timeout,THE_CHUNK_SERVER.get_root_server(),0,process_idx_tid_,start_key,scanner);
              if(ret != OB_SUCCESS)
              {
                TBSYS_LOG(WARN,"fetch location failed,no tablet in root_table,ret=%d",ret);
                need_release_all_tablet = true;
                break;
              }
              else
              {
                ret = parse_location_from_scanner(scanner, start_key, process_idx_tid_, need_other_cs);
              }
              if (ret != OB_SUCCESS)
              {
                TBSYS_LOG(WARN, "parse tablet info from ObScanner failed, ret=[%d]", ret);
                need_release_all_tablet = true;
                break;
              }
              else if (ObRowkey::MAX_ROWKEY == start_key)
              {
                TBSYS_LOG(INFO, "get all tablets info from rootserver success");
                break;
              }
              else
              {
                TBSYS_LOG(DEBUG, "need more request to get next tablet info");
                scanner.reset();
              }
            }
            while(true);
          }
        }
        hash::ObHashMap<ObNewRange, ObTabletLocationList,hash::NoPthreadDefendMode>::const_iterator iter = range_hash_.begin();
        RangeRecord record;
        for (;iter != range_hash_.end(); ++iter)
        {
          if(0 == range_array_.count())
          {
            record.range_ = iter->first;
            range_array_.push_back(record);
          }
          else
          {
            bool in_array = false;
            for(int64_t i = 0;i < range_array_.count();i++)
            {
              if(iter->first == range_array_.at(i).range_)
              {
                in_array = true;
                break;
              }
            }
            if(!in_array)
            {
              record.range_ = iter->first;
              range_array_.push_back(record);
            }
          }
        }
      }
      return ret;
    }
    // add e

    //这个接口有点问题
    int ObIndexHandlePool::fetch_tablet_info(bool is_local_index, bool need_other_cs)
    {
      int ret = OB_SUCCESS;
      const int64_t timeout = 2000000;
      //THE_CHUNK_SERVER.get_client_manager().send_request()
      //int64_t version = schema_mgr_->get_latest_version();
      //TBSYS_LOG(INFO,"test::whx version[%ld]",version);
      const ObSchemaManagerV2* schema = schema_mgr_->get_schema(process_idx_tid_);
      const ObTableSchema* index_schema = NULL;
      uint64_t ref_table_id = OB_INVALID_ID;
      ObVector<ObTablet*> tablet_list;
      bool need_release_all_tablet = false;
      //int64_t size = 0;
      if(NULL == schema)
      {
        TBSYS_LOG(ERROR, "failed in take schema!");
        ret = OB_SCHEMA_ERROR;
      }
      else if(NULL == (index_schema = schema->get_table_schema(process_idx_tid_)))
      {
        TBSYS_LOG(ERROR, "failed in find schema[%ld]",process_idx_tid_ );
        ret = OB_SCHEMA_ERROR;
      }
      else if(is_local_index)
      {
        ref_table_id = index_schema->get_original_table_id();
      }
      else
      {
        ref_table_id = process_idx_tid_;
      }
      if(OB_SUCCESS == ret)
      {
       // TBSYS_LOG(INFO,"test::whx size[%ld]",tablet_array_.count());
        {
          //ref_table_id = index_schema->get_index_helper().tbl_tid;
          if (is_local_index
              && OB_SUCCESS
                  != (ret =
                      tablet_manager_->get_serving_tablet_image().get_serving_image().acquire_tablets_by_table_id(
                          ref_table_id, tablet_list)))
          {
            TBSYS_LOG(ERROR, "get tablets error!");
            ret = OB_ERROR;
          }

          if(OB_SUCCESS == ret)
          {
            ObGeneralRpcStub rpc_stub=THE_CHUNK_SERVER.get_rpc_stub();
            ObScanner scanner;
            ObRowkey start_key;
            range_hash_.clear();
            if(is_local_index && need_other_cs)
            {
              data_multcs_range_hash_.clear();
            }
            start_key.set_min_row();
            {
              do{
                  ret = rpc_stub.fetch_tablet_location(timeout,THE_CHUNK_SERVER.get_root_server(),0,ref_table_id,start_key,scanner);
                  if(ret != OB_SUCCESS)
                  {
                    TBSYS_LOG(WARN,"fetch location failed,no tablet in root_table,ret=%d",ret);
                    need_release_all_tablet = true;
                    break;
                  }
                  else
                  {
                    ret = parse_location_from_scanner(scanner, start_key,ref_table_id, need_other_cs);
                  }
                  if (ret != OB_SUCCESS)
                  {
                    TBSYS_LOG(WARN, "parse tablet info from ObScanner failed, ret=[%d]", ret);
                    need_release_all_tablet = true;
                    break;
                  }
                  else if (ObRowkey::MAX_ROWKEY == start_key)
                  {
                    TBSYS_LOG(INFO, "get all tablets info from rootserver success");
                    break;
                  }
                  else
                  {
                    TBSYS_LOG(DEBUG, "need more request to get next tablet info");
                    scanner.reset();
                  }
                }
               while(true);
           }
           if(OB_SUCCESS == ret)
           {
             if(is_local_index && ! need_other_cs)
             {
               bool need_index = false;
               for(ObVector<ObTablet*>::iterator it = tablet_list.begin(); it != tablet_list.end(); ++it)
               {
                 /*if(is_need_static_index_tablet(it))
                 {
                   tablet_array_[tablets_num_]=it;
                   tablets_num_++;
                 }
                 */
                 ObTabletLocationList list;
                 bool is_handled = false;
                 if(OB_SUCCESS == (ret = is_tablet_handle(*it, is_handled)))
                 {
                   if(!is_handled)
                   {
                     if(OB_SUCCESS != (ret = is_tablet_need_build_static_index(*it, list, need_index)))
                     {
                       TBSYS_LOG(ERROR,"error in is_need_static_index_tablet,ret[%d]",ret);
                     }
                     else if(need_index)
                     {
                       //TBSYS_LOG(INFO,"test::whx add tablet in array");
                       TabletRecord record;
                       record.tablet_ = *it;
                       tablet_array_.push_back(record);
                     }
                     //add liumz, bugfix: tablet image 20150626:b
                     else if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(*it)))
                     {
                       TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                       //break;//del liumz
                     }
                     //add:e
                   }
                   else if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(*it)))
                   {
                     TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                     //break;//del liumz
                   }
                   /*
                   else
                   {
                     //TBSYS_LOG(ERROR,"test::whx release tablet");
                   }*/
                 }
               }
             }
             else if(is_local_index && need_other_cs)
             {
               for(ObVector<ObTablet*>::iterator it = tablet_list.begin(); it != tablet_list.end(); ++it)
               {
                 if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(*it)))
                 {
                   TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                   break;
                 }
               }
             }
             else if(!is_local_index)
             {
               hash::ObHashMap<ObNewRange, ObTabletLocationList,hash::NoPthreadDefendMode>::const_iterator iter = range_hash_.begin();
               RangeRecord record;
               for (;iter != range_hash_.end(); ++iter)
               {
                 if(0 == range_array_.count())
                 {
                   record.range_ = iter->first;
                   range_array_.push_back(record);
                 }
                 else
                 {
                   bool in_array = false;
                   for(int64_t i = 0;i < range_array_.count();i++)
                   {
                     if(iter->first == range_array_.at(i).range_)
                     {
                       in_array = true;
                       break;
                     }
                   }
                   if(!in_array)
                   {
                     record.range_ = iter->first;
                     range_array_.push_back(record);
                   }
                 }
                 //TBSYS_LOG(INFO,"test::whx range_array_count(%ld)",range_array_.count());
;               }
             }
           }
           else if(need_release_all_tablet)//not success
           {
             for(ObVector<ObTablet*>::iterator it = tablet_list.begin(); it != tablet_list.end(); ++it)
             {
               if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(*it)))
               {
                 TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                 break;
               }
             }
           }
         }
       }
     }
     if(NULL != schema)
     {
       if(OB_SUCCESS != (schema_mgr_->release_schema(schema)))
       {
         TBSYS_LOG(ERROR, "failed to release schema ret[%d]", ret);
       }
     }
     return ret;
    }

    int ObIndexHandlePool::prepare_build_total_index()
    {
      int ret = OB_SUCCESS;

      return ret;
    }

    int ObIndexHandlePool::set_config_param()
    {
      int ret = OB_SUCCESS;
      if(OB_SUCCESS != (ret = data_multcs_range_hash_.create(hash::cal_next_prime(512))))
      {
        TBSYS_LOG(ERROR,"init data range hash error,ret=%d",ret);
      }
      if(OB_SUCCESS != (ret = range_hash_.create(hash::cal_next_prime(512))))
      {
        TBSYS_LOG(ERROR,"init index range hash error,ret=%d",ret);
      }
      else
      {
        //reset_failed_record();
      }
      return ret;
    }

    int ObIndexHandlePool::parse_location_from_scanner(ObScanner &scanner, ObRowkey &row_key, uint64_t table_id, bool need_other_cs)
    {
      int ret = OB_SUCCESS;
      ObRowkey start_key;
      start_key = ObRowkey::MIN_ROWKEY;
      ObRowkey end_key;
      ObServer server;
      ObCellInfo * cell = NULL;
      bool row_change = false;
      ObTabletLocationList list;
      //CharArena allocator;
      ObScannerIterator iter = scanner.begin();
      ObNewRange range;
      ++iter;

      scanner.dump_all(TBSYS_LOG_LEVEL_ERROR);//test::lmz

      while ((iter != scanner.end())
          && (OB_SUCCESS == (ret = iter.get_cell(&cell, &row_change))) && !row_change)
      {
        if (NULL == cell)
        {
          ret = OB_INNER_STAT_ERROR;
          break;
        }
        cell->row_key_.deep_copy(start_key, allocator_);
        ++iter;
      }

      if (ret == OB_SUCCESS)
      {
        int64_t ip = 0;
        int64_t port = 0;
        // next cell
        for(++iter; iter != scanner.end(); ++iter)
        {
          ret = iter.get_cell(&cell,&row_change);
          if(ret != OB_SUCCESS)
          {
            TBSYS_LOG(ERROR, "get cell from scanner iterator failed:ret[%d]", ret);
            break;
          }
          else if (row_change) // && (iter != last_iter))
          {
            construct_tablet_item(table_id,start_key, end_key, range, list);
            list.print_info();//test::lmz
            TBSYS_LOG(ERROR,"test::lmz, list[0]'s ip [%d], self ip[%d]", list[0].server_.chunkserver_.get_ipv4(), THE_CHUNK_SERVER.get_self().get_ipv4());
            if(need_other_cs)
            {
              if(-1 ==  data_multcs_range_hash_.set(list.get_tablet_range(), list, 1))
              {
                TBSYS_LOG(ERROR,"insert data_multcs_range_hash_ error!");
              }
            }
            else if(list[0].server_.chunkserver_.get_ipv4() == THE_CHUNK_SERVER.get_self().get_ipv4())
            {
             if(-1  ==  range_hash_.set(list.get_tablet_range(), list, 1))
             {
               TBSYS_LOG(ERROR,"insert range_hash_ error!");
             }
            }
            list.clear();
            start_key = end_key;
          }
          else
          {
            cell->row_key_.deep_copy(end_key, allocator_);
            if ((cell->column_name_.compare("1_port") == 0)
                || (cell->column_name_.compare("2_port") == 0)
                || (cell->column_name_.compare("3_port") == 0)
                    )
            {
              ret = cell->value_.get_int(port);
            }
            else if ((cell->column_name_.compare("1_ipv4") == 0)
                || (cell->column_name_.compare("2_ipv4") == 0)
                || (cell->column_name_.compare("3_ipv4") == 0)
                     )
            {
              ret = cell->value_.get_int(ip);
              if (OB_SUCCESS == ret)
              {
                if (port == 0)
                {
                  TBSYS_LOG(WARN, "check port failed:ip[%ld], port[%ld]", ip, port);
                }
                server.set_ipv4_addr(static_cast<int32_t>(ip), static_cast<int32_t>(port));
                ObTabletLocation addr(0, server);
                if (OB_SUCCESS != (ret = list.add(addr)))
                {
                  TBSYS_LOG(ERROR, "add addr failed:ip[%ld], port[%ld], ret[%d]",
                      ip, port, ret);
                  break;
                }
                else
                {
                  TBSYS_LOG(DEBUG, "add addr succ:ip[%ld], port[%ld], server:%s", ip, port, to_cstring(server));
                }
                ip = port = 0;
              }
            }
            if (ret != OB_SUCCESS)
            {
              TBSYS_LOG(ERROR, "check get value failed:ret[%d]", ret);
              break;
            }
          }
        }
        // for the last row
        TBSYS_LOG(DEBUG, "get a new tablet start_key[%s], end_key[%s]",
            to_cstring(start_key), to_cstring(end_key));
        if ((OB_SUCCESS == ret) && (start_key != end_key))
        {
          construct_tablet_item(table_id, start_key, end_key, range, list);
      list.print_info();//test::lmz
          TBSYS_LOG(ERROR,"test::lmz, list[0]'s ip [%d], self ip[%d]", list[0].server_.chunkserver_.get_ipv4(), THE_CHUNK_SERVER.get_self().get_ipv4());
          if(need_other_cs)
          {
            if(-1 ==  data_multcs_range_hash_.set(list.get_tablet_range(), list, 1))
            {
              TBSYS_LOG(ERROR,"insert data_multcs_range hash error!");
              ret = OB_ERROR;
            }
          }
          else if(list[0].server_.chunkserver_.get_ipv4() == THE_CHUNK_SERVER.get_self().get_ipv4())
          {
            if(-1 == range_hash_.set(list.get_tablet_range(),list,1))
            {
              TBSYS_LOG(ERROR,"insert range hash error!");
              ret = OB_ERROR;
            }
          }
        }
      }
      else
      {
        TBSYS_LOG(ERROR, "check get first row cell failed:ret[%d]", ret);
      }
      if(OB_SUCCESS == ret)
      {
        row_key = end_key;
      }

      return ret;
    }

    int ObIndexHandlePool::is_tablet_need_build_static_index(ObTablet *tablet, ObTabletLocationList &list, bool &is_need_index)
    {
      int ret = OB_SUCCESS;
      is_need_index = false;
      if(NULL == tablet)
      {
        TBSYS_LOG(ERROR,"null pointer for tablet");
        ret = OB_ERROR;
      }
      else
      {
        ObNewRange range = tablet->get_range();
        //char str[1024] = {0};
        //range.to_string(str,1024);
       // TBSYS_LOG(INFO,"test::whx range_str=%s",str);
        if(hash::HASH_EXIST == range_hash_.get(range,list))
        {
          is_need_index = true;
        }
        else
        {
          is_need_index = false;
        }
      }
      //TBSYS_LOG(INFO,"test::whx sstable count=%ld",tablet->get_sstable_id_list().count());
      if(is_need_index && tablet->get_sstable_id_list().count() == ObTablet::MAX_SSTABLE_PER_TABLET)
      {
        is_need_index = false;
      }
      return ret;
    }

    int ObIndexHandlePool::finish_phase1(bool &reported)
    {
      int ret = OB_SUCCESS;
      if(0 == static_index_report_infolist->tablet_list_.get_array_index())
      {

      }
      else if(OB_SUCCESS != (ret = send_local_index_info()) )
      {
        TBSYS_LOG(ERROR,"report index info failed, ret[%d]", ret);
        static_index_report_infolist->reset();
        for(int i = 0; i<10; i++)
        {
          handler_[i]->get_allocator()->reuse();   //发送失败,释放内存
          handler_[i]->reset_report_info();
        }
        TBSYS_LOG(WARN,"partional tablet histogram info report failed, memory reuse");
      }
      else
      {
        reported = true;
        static_index_report_infolist->reset();
        for(int i = 0; i<10; i++)
        {
          handler_[i]->get_allocator()->reuse();   //发送成功,释放内存d
          handler_[i]->reset_report_info();
        }
        TBSYS_LOG(INFO,"partional tablet histogram info report success, memory reuse");
      }

      return ret;
    }

    int ObIndexHandlePool::finish_phase2(bool & total_reported)
    {
      int ret = OB_SUCCESS;
      pthread_mutex_lock(&phase_mutex_);
      if(ROUND_FALSE >= round_end_)
      {
        if (OB_RESPONSE_TIME_OUT == (ret = tablet_manager_->report_index_tablets(process_idx_tid_)))
        {
          TBSYS_LOG(WARN,"send index tablets info failed = %d",ret);
        }
        else
        {
          total_reported = true;
          tablet_manager_->report_capacity_info();
        }
      }
      if(OB_SUCCESS == ret && ROUND_FALSE >= round_end_)
      {
        ret = tablet_manager_->get_serving_tablet_image().get_serving_image().delete_local_index_sstable();
        //TBSYS_LOG(ERROR,"test::whx delete tablet!");
      }
      if(OB_SUCCESS == ret && ROUND_FALSE >= round_end_)
      {
        round_end_ = ROUND_TRUE;
        round_start_ = ROUND_FALSE;

      }
      pthread_mutex_unlock(&phase_mutex_);
      return ret;
    }

    //add wenghaixing [secondary index static_sstable_build.report]20150316
    int ObIndexHandlePool::add_tablet_info(ObTabletReportInfo *tablet)
    {
      int ret = OB_SUCCESS;
      ObNewRange copy_range;
      //alloc_mutex_.lock();
      ObTabletReportInfo copy;
      copy = *tablet;
      //modify liuxiao [secondary index static_index_build.bug_fix.merge_error]20150604
      if(NULL == tablet)
      {
        ret = OB_ERROR;
        TBSYS_LOG(ERROR, "tablet is null");
      }
      else if (OB_SUCCESS != (ret = deep_copy_range(report_allocator_, tablet->tablet_info_.range_, copy_range)))
      //if ( OB_SUCCESS != (ret = deep_copy_range(report_allocator_, tablet->tablet_info_.range_, copy_range)) )
      //modify e
      {
        TBSYS_LOG(ERROR, "copy range failed.");
      }
      else
      {
        copy.tablet_info_.range_ = copy_range;
      }
      // alloc_mutex_.unlock();
      if(OB_SUCCESS != ret || NULL == tablet)
      {
        TBSYS_LOG(ERROR, "failed to add tablet report info for sstable local index,null pointer");
        ret = OB_ERROR;
      }
      else
      {
        ret = report_info_.add_tablet(copy);
      }

      return ret;
    }

    int ObIndexHandlePool::send_local_index_info()
    {
      int ret = OB_SUCCESS;
      //add zhuyanchao[secondary index static index build]
      int64_t histogram_index =0;
      const int64_t REPORT_INFO_LIST_RESERVED_SERIALIZE_SIZE = 8;
      int64_t max_serialize_size = OB_MAX_PACKET_LENGTH - 1024;
      int64_t serialize_size = REPORT_INFO_LIST_RESERVED_SERIALIZE_SIZE;
      ObTabletHistogramReportInfoList* report_list =OB_NEW(ObTabletHistogramReportInfoList,ObModIds::OB_TABLET_HISTOGRAM_REPORT);
      ObTabletHistogramReportInfo* report_info=OB_NEW(ObTabletHistogramReportInfo,ObModIds::OB_TABLET_HISTOGRAM_REPORT);
      common::ObTabletHistogramReportInfoList *tablet_histogram_report_info_list=get_ob_tablet_histogram_report_info_list();
      //add test zhuyanchao
     // int64_t test = tablet_histogram_report_info_list->tablet_list_.get_array_index();
     // TBSYS_LOG(WARN,"test::zhuyanchao histogram report info size=%ld",test);
      //add e
      if(NULL == report_list || NULL == report_info)
      {
         ret =OB_ALLOCATE_MEMORY_FAILED;
      }
      else
      {
         report_list->reset();
      }

      while (OB_SUCCESS == ret)
      {
        while (OB_SUCCESS == ret &&histogram_index <tablet_histogram_report_info_list->tablet_list_.get_array_index())
        {
         // TBSYS_LOG(ERROR,"test::zhuyanchao begin to report info");
          ret=tablet_histogram_report_info_list->get_tablet_histogram(*report_info,histogram_index);
           //TBSYS_LOG(WARN,"test::zhuyanchao histogram report info table id =%ld",report_info->tablet_info_.range_.table_id_);
          //TBSYS_LOG(ERROR,"test::whx start dump,histogram_index = %ld,array_index = %ld", histogram_index, tablet_histogram_report_info_list->tablet_list_.get_array_index());
          //report_info->static_index_histogram_.dump();

          if(OB_SUCCESS != ret)
          {
            TBSYS_LOG(WARN, "failed to get tablet histogram info, err=%d", ret);
          }
          else
          {
            histogram_index ++;
            ret = report_list->add_tablet(*report_info);
            //TBSYS_LOG(ERROR,"test::zhuyanchao report table id=%ld",report_info->tablet_info_.range_.table_id_);
            if (OB_SUCCESS != ret)
            {
              TBSYS_LOG(WARN, "failed to add tablet histogram info, err=%d", ret);
            }
            else
            {
              serialize_size += report_info->get_serialize_size();
              if (serialize_size > max_serialize_size)
              {
                break;
              }
            }
          }
        }
        if(OB_SUCCESS == ret)
        {
          ret =tablet_manager_->send_tablet_histogram_report(*report_list,true);
          if(OB_SUCCESS != ret)
          {
            TBSYS_LOG(WARN, "failed to send tablet histogram info, err=%d", ret);
          }
        }
        if (OB_SUCCESS == ret)
        {
          serialize_size = REPORT_INFO_LIST_RESERVED_SERIALIZE_SIZE;
          //num = OB_MAX_TABLET_LIST_NUMBER;
          report_list->reset();
        }
      }
      if(OB_ITER_END == ret)
      {
        //TBSYS_LOG(WARN, "test::zhuyanchao test send last histogram info");
        ret = report_list->add_tablet(*report_info);
        if (OB_SUCCESS != ret)
        {
        TBSYS_LOG(WARN, "failed to add tablet histogram info, err=%d", ret);
        }
        ret =tablet_manager_->send_tablet_histogram_report(*report_list,false);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(WARN, "failed to send tablet histogram report in last round, ret=%d", ret);
        }
      }
      /*for(int i=0;i<MAX_WORK_THREAD; i++)
      {
          handler_[i]->get_allocator()->reuse();
      }*/
      OB_DELETE(ObTabletHistogramReportInfoList,ObModIds::OB_TABLET_HISTOGRAM_REPORT,report_list);
      OB_DELETE(ObTabletHistogramReportInfo,ObModIds::OB_TABLET_HISTOGRAM_REPORT,report_info);
      //add e
      //ret = tablet_manager_->send_tablet_report(report_info_, false);

      return ret;
    }
    //add e
    int ObIndexHandlePool::get_tablets_ranges(TabletRecord *&tablet, RangeRecord *&range, int &err)
    {
      int ret = OB_SUCCESS;
      bool reported = false;
      bool total_reported = false;
      err = OB_GET_NOTHING;
      UNUSED(range);
      //int err = OB_ERROR;
      tablet = NULL;
      range = NULL;
      if(tablet_array_.count() > 0 && tablet_index_ < tablet_array_.count())
      {
        TBSYS_LOG(INFO,"get tablet from local list,tablet_index_:%ld,tablets_num_:%ld",tablet_index_,tablet_array_.count());
        tablet = &(tablet_array_.at(tablet_index_++));
        err = OB_GET_TABLETS;
      }
      //liumz, 多线程已经处理完tablet_array_.count()个tablet
      else if(tablets_have_got_ == tablet_array_.count() && tablets_have_got_ != 0)
      {
        if(check_if_tablet_range_failed(true,tablet,range))//检查是否有失败的任务，有的话，赋值，继续完成
        {
          err = OB_GET_TABLETS;
        }
        else if (is_phase_one_need_end() && OB_SUCCESS == (ret = finish_phase1(reported)))
        {
          if(reported)
          {
          TBSYS_LOG(INFO,"report local index tablet success!");
          }
        }
        //todo
        ///继续尝试向rs获取构建局部索引的range，如果没有，则返回ret = OB_GET_NOTHING;
      }
      ///todo if(range_num == 0 && prepare_range_to_build)
      /// {
      ///  |-- >首先尝试去rootserver取全局索引的range，如果没有得到，则说明还处于局部索引的构建阶段
      ///
      /// }
      /// else
      /// {
      ///  |-- >如果得到了一个range，则对range进行赋值，并返回ret = OB_GET_RANGES;
      ///  |-- >如果range取完，则先汇报，之后再尝试去取一个range
      /// }
      if(OB_GET_NOTHING == err)
      {
        if(0 < range_array_.count()&&range_index_ < range_array_.count())
        {
          range = &range_array_.at(range_index_ ++);
          err = OB_GET_RANGES;
        }
        else if(range_have_got_ == range_array_.count() && range_have_got_ != 0)
        {
          if(check_if_tablet_range_failed(false,tablet,range)) //检查是否有失败的任务，有的话，赋值，继续完成
          {
            err = OB_GET_RANGES;
          }
          else if (is_phase_two_need_end() && OB_SUCCESS == (ret = finish_phase2(total_reported)))
          {
            if(total_reported)
            {
            TBSYS_LOG(INFO,"report total index tablet success!");
            }
          }
        }
      }
      //TBSYS_LOG(ERROR,"test::whx ret[%d],err[%d]",ret,err);
      return ret;
    }

    int ObIndexHandlePool::release_tablet_array()
    {
      int ret = OB_SUCCESS;
      ObTablet *tablet = NULL;
      for(int64_t i = 0; i < tablet_array_.count(); i++)
      {
        tablet = tablet_array_.at(i).tablet_;
        if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(tablet)))
        {
          TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
          break;
        }
      }
      if(OB_SUCCESS == ret)
      {
        round_end_ = TABLET_RELEASE;
      }
      return ret;
    }

    void ObIndexHandlePool::inc_get_tablet_count()
    {
      if(tablet_array_.count() > tablets_have_got_)
      {
        tablets_have_got_++;
      }
      else
      {
        TBSYS_LOG(INFO, "all tablet has been consume, check if has failed record");
      }
    }

    void ObIndexHandlePool::inc_get_range_count()
    {
      if(range_array_.count() > range_have_got_)
      {
        range_have_got_ ++;
      }
      else
      {
        TBSYS_LOG(INFO, "all range has been consume , check if has failed record");
      }
    }
    int ObIndexHandlePool::check_self()
    {
      int ret = OB_SUCCESS;
      bool need_signal = false;
      bool is_local_index = false;
      bool need_other_cs = false;
      //add wenghaixing [secondary index cluster.p2]20150630
      int64_t status = -1;
      //add e
      if(NULL == tablet_manager_)
      {
        TBSYS_LOG(ERROR, "tablet manager cannot be NULL!");
        ret = OB_INVALID_ARGUMENT;
      }
      if(OB_SUCCESS == ret && total_work_last_end_time_ != 0 && 0 == active_thread_num_ )
      {
        ret = release_tablet_array();
        TBSYS_LOG(INFO,"index build failed, release all resource!");
        if(OB_SUCCESS == ret)
        {
          reset();
        }
        return ret;
      }
      //add wenghaixing [secondary index cluster.p2]20150630
      if(OB_SUCCESS == ret && 0 == active_thread_num_ && OB_INVALID_ID != process_idx_tid_)
      {
        ret = THE_CHUNK_SERVER.get_rpc_stub().fetch_index_stat(THE_CHUNK_SERVER.get_config().network_timeout, THE_CHUNK_SERVER.get_root_server(), (int64_t)process_idx_tid_,
                                                         THE_CHUNK_SERVER.get_config().cluster_id,status);

      }

      if(OB_SUCCESS == ret && is_current_index_failed(status) && 0 == active_thread_num_ && OB_INVALID_ID != process_idx_tid_)
      {
        ret = release_tablet_array();
        TBSYS_LOG(INFO,"index build failed, release all resource!");
        if(OB_SUCCESS == ret)
        {
          reset();
        }
      }
      else if(OB_SUCCESS == ret && OB_INVALID_ID != process_idx_tid_ && is_current_index_complete(status) && 0 == active_thread_num_ && TABLET_RELEASE != round_end_)
      {
        ret = release_tablet_array();
        if(OB_SUCCESS == ret)
        {
          reset();
        }
      }
      else if(OB_SUCCESS == ret && OB_INVALID_ID != process_idx_tid_ && !is_current_index_complete(status) && 0 == active_thread_num_)
      {
        if(true)
        {
          //check_ = true;
          int64_t tablet_counter = tablet_array_.count();
          int64_t range_counter = range_array_.count();
          if(tablets_have_got_ == tablet_array_.count() && OB_INVALID_ID != process_idx_tid_ && !need_signal)
          {
            //usleep(SLEEP_INTERVAL);
           // tablet_index_ = 0;
            if(OB_SUCCESS != (ret = fetch_tablet_info()))
            {
              TBSYS_LOG(WARN,"fetch talet info failed,ret[%d]", ret);
            }
            else if(tablet_counter < tablet_array_.count() )
            {
              need_signal = true;

            }
            else if(OB_INVALID_ID != process_idx_tid_)
            {
              TBSYS_LOG(INFO,"no tablet to build, sleep[%ld] wait it", SLEEP_INTERVAL);
            }
          }

          if(range_have_got_ == range_array_.count() && (OB_INVALID_ID != process_idx_tid_) && !need_signal)
          {
            //usleep(SLEEP_INTERVAL);
            is_local_index = false;
            need_other_cs = false;
            //range_index_ = 0;
            if(OB_SUCCESS != (ret = fetch_tablet_info(is_local_index,need_other_cs)))
            {
              TBSYS_LOG(WARN,"fetch talet info failed,ret[%d]", ret);
            }
            else if(range_counter < range_array_.count())
            {
              is_local_index = true;
              need_other_cs = true;
              if(OB_SUCCESS != (ret = fetch_tablet_info(is_local_index,need_other_cs)))
              {
                TBSYS_LOG(WARN,"fetch talet info failed,ret[%d]", ret);
              }
              else
              {
                need_signal = true;


              }
            }
            else if(OB_INVALID_ID != process_idx_tid_)
            {
              TBSYS_LOG(INFO,"no range to build, sleep[%ld] wait it", SLEEP_INTERVAL);
            }
          }

          if(range_have_got_ == range_array_.count()&& tablets_have_got_ == tablet_array_.count() && !need_signal
             &&!black_list_array_.is_list_array_bleach()
             )
          {
            BlackList list;
            ObNewRange range;
            ObTablet *tablet = NULL;
            do
            {
              ret = black_list_array_.get_next_black_list(list);
              if(OB_SUCCESS ==  ret)
              {
                list.get_range(range);
                if(process_idx_tid_ == range.table_id_)
                {
                  RangeRecord record;
                  record.range_ = range;
                  ret = range_array_.push_back(record);
                }
                else
                {
                   ret = tablet_manager_->get_serving_tablet_image().get_serving_image().acquire_tablet(range, ObMultiVersionTabletImage::SCAN_FORWARD, tablet);
                   if(OB_SUCCESS == ret)
                   {
                     bool is_handle = false;
                     if(OB_SUCCESS == (ret = is_tablet_handle(tablet, is_handle)))
                     {
                       if(!is_handle)
                       {
                         TabletRecord record;
                         record.tablet_ = tablet;
                         tablet_array_.push_back(record);
                         need_signal = true;
                       }
                       else if(OB_SUCCESS !=(ret = tablet_manager_->get_serving_tablet_image().release_tablet(tablet)))
                       {
                         TBSYS_LOG(WARN, "release tablet array failed, ret = [%d]", ret);
                         break;
                       }
                     }
                   }
                   else
                   {
                     TBSYS_LOG(ERROR, "get tablet from tablet_manager failed.ret = [%d]", ret);
                   }
                }
              }
            }while(OB_SUCCESS == ret);

            if(OB_ITER_END == ret)
            {
              ret = OB_SUCCESS;
            }
          }

          if(/*OB_SUCCESS == ret && */need_signal)
          {
            pthread_cond_broadcast(&cond_);
          }
          //check_ = false;
        }
      }
      return ret;
    }

    int ObIndexHandlePool::get_index_handler(const int64_t thread_no, ObIndexHandler *&handler)
    {
      int ret = OB_SUCCESS;
      handler = NULL;
      ObIndexHandler** handlers = NULL;
      handlers = handler_;

      if(thread_no >= MAX_WORK_THREAD)
      {
        TBSYS_LOG(ERROR,  "thread_no=%ld >= max_work_thread_num=%ld", thread_no, MAX_WORK_THREAD);
        ret = OB_SIZE_OVERFLOW;
      }
      else if (NULL == handlers)
      {
        TBSYS_LOG(ERROR, "thread_no=%ld handlers is NULL", thread_no);
        ret = OB_SIZE_OVERFLOW;
      }
      else if (NULL == (handler = handlers[thread_no]))
      {
        TBSYS_LOG(ERROR, "thread_no=%ld handlers is NULL", thread_no);
        ret = OB_INVALID_ARGUMENT;
      }

      return ret;

    }
//add wenghaixing [secondary index static_index_build cluster.p2]20150630
    bool ObIndexHandlePool::is_current_index_failed(const int64_t status)
    {
      bool ret = true;
      //int64_t now = tbsys::CTimeUtil::getTime();
      //int64_t nernal = THE_CHUNK_SERVER.get_config().build_index_timeout;
      //TBSYS_LOG(WARN, "build local static index timeout, mission_start_time_=%ld, now=%ld,ternal[%ld]", local_work_start_time_, now, nernal);
      if(OB_INVALID_ID == process_idx_tid_)
      {
        ret = false;
        //TBSYS_LOG(INFO,"this is first round of index construction!");
      }
      /*else if(now > local_work_start_time_ + THE_CHUNK_SERVER.get_config().build_index_timeout)
      {
        ret = true;
        TBSYS_LOG(WARN, "build local static index timeout, mission_start_time_=%ld, now=%ld.", local_work_start_time_, now);
      }*/
      else
      {
        ret = ERROR == status ? true : false;
      }

      return ret;
    }

    bool ObIndexHandlePool::is_current_index_complete(const int64_t status)
    {
      bool ret = false;
      if(OB_INVALID_ID == process_idx_tid_)
      {
        ret = true;
        //TBSYS_LOG(INFO,"this is first round of index construction!");
      }
      else //if(round_end_ > ROUND_FALSE)//del liumz, bugfix: tablet image 20150626
      {
        ret = (NOT_AVALIBALE == status || AVALIBALE == status)? true : false;
      }

      return ret;
    }
    //add e

    // add longfei [cons static index] 151123:b
    int ObIndexHandlePool::construct_index(const int64_t thread_no)
    {
      int ret = OB_SUCCESS;
      if (common::ConIdxStage::LOCAL_INDEX_STAGE == get_which_stage())
      {
        // construct local index stage
      }
      else if (common::ConIdxStage::GLOBAL_INDEX_STAGE == get_which_stage())
      {
        // construct global index stage
      }
      else
      {
        ret = OB_ERROR;
        TBSYS_LOG(ERROR,"stage message error");
      }

    }

    void ObIndexHandlePool::construct_index(const int64_t thread_no)
    {
      int ret = OB_SUCCESS;
      int err = OB_SUCCESS;
      const int64_t sleep_interval = 5000000;
      TabletRecord *tablet = NULL;
      RangeRecord *range = NULL;
      ObIndexHandler * handler=NULL;

      while(true)
      {
        if(!inited_)
        {
          break;
        }

        pthread_mutex_lock(&mutex_);
        //if(phase1_ && !phase2_)
        {
          //TBSYS_LOG(INFO,"test::whx try to get a tablet/range to build index");
          ret = get_tablets_ranges(tablet, range, err);
        }
        while(true)
        {
          if(OB_GET_NOTHING == err && OB_SUCCESS != ret)
          {
            pthread_mutex_unlock(&mutex_);
            usleep(sleep_interval);
            // retry to get tablet until got one or got nothing.
            pthread_mutex_lock(&mutex_);
          }
          else if(OB_GET_NOTHING == err)
          //因为处于测试版本，所以当得不到任何东西时就该阻塞线程
          //else if(OB_GET_NOTHING == err && is_current_index_complete())
          //而实际情况里，直到确定索引表构建成功，才会阻塞线程
          {
            --active_thread_num_;
            TBSYS_LOG(INFO,"there is no tablet need build static index,sleep wait for new index process.");
            pthread_cond_wait(&cond_,&mutex_);
            TBSYS_LOG(INFO,"awake by signal,active_thread_num_=:%ld",active_thread_num_);
            ++active_thread_num_;
          }
          else
          {
            break;
          }
          //if(phase1_ && !phase2_)
          {
            ret = get_tablets_ranges(tablet, range, err);
          }
        }
        pthread_mutex_unlock(&mutex_);

        if(OB_GET_TABLETS == err)//如果是拿到的tablet，则就是出于局部索引的构建阶段
        {
          //TBSYS_LOG(INFO,"test::whx the tablet is not null and range is %s",to_cstring(tablet->tablet_->get_range()));
          if(OB_SUCCESS != (ret = get_index_handler(thread_no, handler)))
          {
            TBSYS_LOG(ERROR, "get index handler error, ret[%d]",ret);
          }
          else
          {
            int err_local = OB_SUCCESS;
            TBSYS_LOG(INFO,"now index handler begin to work!");
            //modify zhuyanchao [secondary index static_sstable_build.report]20150320
            //TBSYS_LOG(WARN,"test::zhuyanchao test histogram list size = %ld",static_index_report_infolist->tablet_list_.get_array_index());
            if(OB_SUCCESS != (err_local = handler->start(tablet->tablet_,hist_width_)))//将sample_rate加入了进去zhuyanchao 20150320
            {
              //add zhuyanchao [secondary index static_sstable_build.report]20150320
              //handler->reset_report_info();
              //add e
              TBSYS_LOG(WARN, "build partitional index failed,tablet[%s],fail count[%d],if_process[%d],err[%d]",to_cstring(tablet->tablet_->get_range()), (int)tablet->fail_count_,(int)tablet->if_process_,err_local);
              tablet->if_process_ = 0;
              tablet->fail_count_++;
              if(tablet->fail_count_ > MAX_FAILE_COUNT && !tablet->wok_send_)
              {
                //todo 如果失败超过一定次数的处理方法
                ///开始甩锅给别的CS
                //bool is_local_index = true;
                ObNewRange no_use_range;
                if(OB_SUCCESS != (err_local = whipping_wok(LOCAL_INDEX_SST_BUILD_FAILED, tablet->tablet_, no_use_range)))
                {
                  ret = OB_INDEX_BUILD_FAILED;
                  tablet->if_process_ = 1;
                  TBSYS_LOG(WARN, "whipping wok failed! err = [%d]",err_local);
                }
                else
                {
                  tablet->wok_send_ = 1;
                  tablet->fail_count_ = 0;
                  tablet->if_process_ = -1;
                }

                TBSYS_LOG(WARN,"tablet failed too much");
              }
            }
            else
            {

              //add zhuyanchao [secondary index static_index_build.report]20150320
              if(OB_SUCCESS != (ret = static_index_report_infolist->add_tablet(*(handler->get_tablet_histogram_report_info()))))//一个tablet结束将tabletreport信息加入汇报总的数组
              {
                TBSYS_LOG(WARN,"add histogram report info failed = %d",ret);
              }
              //add e
              tablet->fail_count_ = 0;
              tablet->if_process_ = -1;
            }
            //TBSYS_LOG(INFO,"test::whx err_local[%d]",err_local);
          }
          ///TODO,这段代码应该删除
#if 0
          if (tablet_manager_->get_serving_tablet_image().release_tablet(tablet) != OB_SUCCESS)
          {
            TBSYS_LOG(WARN,"release tablet failed");
          }
#endif
          pthread_mutex_lock(&tablet_range_mutex_);
          inc_get_tablet_count();
          //TBSYS_LOG(INFO,"test::whx tablets_have_got_++[%ld]",tablets_have_got_);
          pthread_mutex_unlock(&tablet_range_mutex_);
          if ( tablet_manager_->is_stoped() )
          {
            TBSYS_LOG(WARN,"stop in index");
            ret = OB_ERROR;
          }
        }
        else if(OB_GET_RANGES == err)
        {
          //todo 如果是得到一个range，则处于全局索引构建的阶段
         // TBSYS_LOG(INFO,"test::whx the index range is not null and range is %s",to_cstring(range->range_));
          if(OB_SUCCESS != (ret = get_index_handler(thread_no, handler)))
          {
            TBSYS_LOG(ERROR, "get index handler error, ret[%d]",ret);
          }
          else
          {
            int err_local = OB_SUCCESS;

            TBSYS_LOG(INFO,"now index handler begin to build total index [%s]!",to_cstring(range->range_));
            if(OB_SUCCESS != (err_local = handler->start_total_index(&range->range_)))
            {
              TBSYS_LOG(WARN, "build global index failed,err[%d]",err_local);
              if(OB_TABLET_HAS_NO_LOCAL_SSTABLE == err_local)
              {
                ObNewRange fake_range;
                if(OB_SUCCESS != handler->get_failed_fake_range(fake_range))
                {
                  ret = OB_INDEX_BUILD_FAILED;
                }
                else
                {
                  if(OB_SUCCESS != (err_local = whipping_wok(LOCAL_INDEX_SST_NOT_FOUND, NULL, fake_range)))
                  {
                    ret = OB_INDEX_BUILD_FAILED;
                  }
                  else
                  {
                    //range->if_process_ = 0;
                    range->fail_count_++;
                    range->if_process_ = 0;
                  }
                }
                //we re-construct this block, so that we can controll it until sstable build success
              }
              else
              {
                range->if_process_ = 0;
                range->fail_count_++;
                if(range->fail_count_ > MAX_FAILE_COUNT && !range->wok_send_)
                {
                  //todo 如果失败超过一定次数的处理方法
                  TBSYS_LOG(WARN,"range failed too much");
                  if(OB_SUCCESS != (err_local = whipping_wok(GLOBAL_INDEX_BUILD_FAILED, NULL, range->range_)))
                  {
                    ret = OB_INDEX_BUILD_FAILED;
                    range->if_process_ = 1;
                  }
                  else
                  {
                    range->fail_count_ = 0;
                    range->if_process_ = -1;
                    range->wok_send_   = 1;
                  }
                }
              }
            }
            else
            {
              range->fail_count_ = 0;
              range->if_process_ = -1;
            }

          }
          pthread_mutex_lock(&tablet_range_mutex_);
          inc_get_range_count();
          pthread_mutex_unlock(&tablet_range_mutex_);
          if ( tablet_manager_->is_stoped() )
          {
            TBSYS_LOG(WARN,"stop in index");
            ret = OB_ERROR;
          }

        }
      }

    }

    void ObIndexHandlePool::run(tbsys::CThread *thread, void *arg)
    {
      UNUSED(thread);
      int64_t thread_no = reinterpret_cast<int64_t>(arg);
      construct_index(thread_no);//索引构建阶段
    }

    bool ObIndexHandlePool::can_launch_next_round()
    {
      bool ret = false;
     // int64_t now = tbsys::CTimeUtil::getTime();
     // TBSYS_LOG(ERROR,"test::whx inited_[%d],active_thread_num_[%ld]",(int)inited_,active_thread_num_);
      if (inited_ &&is_work_stoped()
         // && now - total_work_last_end_time_ > THE_CHUNK_SERVER.get_config().min_merge_interval
          //&& THE_CHUNK_SERVER.get_tablet_manager().get_bypass_sstable_loader().is_loader_stoped()
          )
      {
        ret = true;
      }
      return ret;
    }

    void ObIndexHandlePool::construct_tablet_item(const uint64_t table_id,
                               const ObRowkey & start_key, const ObRowkey & end_key, ObNewRange & range,
                               ObTabletLocationList & list)
    {
      range.table_id_ = table_id;
      range.border_flag_.unset_inclusive_start();
      range.border_flag_.set_inclusive_end();
      range.start_key_ = start_key;
      range.end_key_ = end_key;
      if(range.end_key_.is_max_row())
      {
        range.border_flag_.unset_inclusive_end();
      }
      list.set_timestamp(tbsys::CTimeUtil::getTime());
      list.set_tablet_range(range);
      // list.sort(addr);
      // double check add all range->locationlist to cache
      if (range.start_key_ >= range.end_key_)
      {
        TBSYS_LOG(WARN, "check range invalid:start[%s], end[%s]",
              to_cstring(range.start_key_), to_cstring(range.end_key_));
      }
      else
      {
        TBSYS_LOG(DEBUG, "got a tablet:%s, with location list:%ld", to_cstring(range), list.size());
      }
    }

    //add wenghaixing [secondary index static_index_build]20150320
    bool ObIndexHandlePool::check_if_tablet_range_failed(bool is_local_index, TabletRecord *&tablet, RangeRecord *&range)
    {
      bool ret = false;
      tablet = NULL;
      range = NULL;
      //TBSYS_LOG(ERROR,"test::whx check if failed here");
      if(is_local_index)
      {
        for(int64_t i = 0; i < tablet_array_.count(); i++)
        {
          //TBSYS_LOG(ERROR,"test::whx if_process set 1 pashe 1,range = [%s],if_process[%d],fail_count[%d]",to_cstring(tablet_array_.at(i).tablet_->get_range()),(int)tablet_array_.at(i).if_process_,(int)tablet_array_.at(i).fail_count_);
          if(tablet_array_.at(i).fail_count_> 0 && 0 == tablet_array_.at(i).if_process_)
          {
            tablet_array_.at(i).if_process_ = 1;
            tablet = &(tablet_array_.at(i));
            //TBSYS_LOG(ERROR,"test::whx if_process set 1 pashe 1");
            ret = true;
            //local_failed_[i]++;
            break;
          }
        }
      }
      else
      {
        for(int64_t i = 0; i < range_array_.count(); i++)
        {
          if(range_array_.at(i).fail_count_ > 0 && 0 == range_array_.at(i).if_process_)
          {
            range = &range_array_.at(i);
            range_array_.at(i).if_process_ = 1;
            //TBSYS_LOG(ERROR,"test::whx if_process set 1 pashe 2");
            ret = true;
            //local_failed_[i]++;
            break;
          }
        }
      }
      return ret;
    }
    //add e

    ///if a tablet/range construct failed in this cs over 3 times,then other cs would finishi it
    int ObIndexHandlePool::whipping_wok(ErrNo level, const ObTablet *tablet, ObNewRange range)
    {
      int ret = OB_SUCCESS;
      ObTabletLocationList list;
      bool in_list = false;
      int64_t index = 0;
      BlackList black_list;
      ObServer next_server;
      ObNewRange wok_range;
      hash::ObHashMap<ObNewRange,ObTabletLocationList,hash::NoPthreadDefendMode> * range_info = NULL;
      //debug 20150420
      switch(level)
      {
        case LOCAL_INDEX_SST_BUILD_FAILED:
          if(NULL == tablet)
          {
            ret = OB_INVALID_ARGUMENT;
          }
          else
          {
            wok_range = tablet->get_range();
            range_info = &range_hash_;
          }
          break;
        case GLOBAL_INDEX_BUILD_FAILED:
          wok_range = range;
          range_info = &range_hash_;
          break;
        case LOCAL_INDEX_SST_NOT_FOUND:
          wok_range = range;
          range_info = &data_multcs_range_hash_;
          break;
        default:
          ret = OB_INVALID_ARGUMENT;
      }
      if(OB_SUCCESS == ret)
      {
        if(hash::HASH_EXIST != range_info->get(wok_range,list))
        {
          ret = OB_INVALID_ARGUMENT;
          TBSYS_LOG(ERROR, "range hash cannot find range,wok_range[%s],hash_size[%ld]",to_cstring(wok_range),data_multcs_range_hash_.size());
        }
        else
        {
          if(OB_SUCCESS != (ret = black_list_array_.check_range_in_list(wok_range,in_list, index)))
          {
            //never happen
          }
          else if(in_list )
          {
            black_list = black_list_array_.get_black_list(index, ret);
            if(OB_SUCCESS != ret)
            {
              TBSYS_LOG(WARN, "get black list failed, ret = [%d]", ret);
            }
            else if(0 == black_list.get_wok_send())
            {
              black_list.set_server_unserved(THE_CHUNK_SERVER.get_self());
              if(OB_SUCCESS != (ret = black_list.next_replic_server(next_server)))
              {
                TBSYS_LOG(WARN, "all replicate failed!!!");
              }
              else if(OB_SUCCESS != (ret = tablet_manager_->whipping_wok(black_list, next_server)))
              {
                black_list.set_server_unserved(next_server);
                black_list.set_wok_send(0);
                TBSYS_LOG(WARN, "whipping wok failed,ret [%d]", ret);
              }
              else
              {
                black_list.set_wok_send();
              }
            }
          }
          else if(!in_list)
          {
            ///todo 不在list里的时候，写BlackList并发送
            if (OB_SUCCESS != (ret = black_list.write_list(wok_range, list)))
            {
              TBSYS_LOG(WARN, "write wok range in black list failed ret[%d]",ret);
            }
            else if (0 == black_list.get_wok_send())
            {
              //bug 20150420
              black_list.set_server_unserved(THE_CHUNK_SERVER.get_self());
              //bug2 20150420
              if(OB_SUCCESS != (ret = black_list_array_.push(black_list)))
              {
                TBSYS_LOG(WARN,"pus black list in array failed ret [%d] ",ret);
              }
              else if(OB_SUCCESS != (ret = black_list.next_replic_server(next_server)))
              {
                TBSYS_LOG(WARN, "all replicate failed!!!");
              }
              else if(OB_SUCCESS != (ret = tablet_manager_->whipping_wok(black_list, next_server)))
              {
                black_list.set_wok_send(0);
                black_list.set_server_unserved(next_server);
                TBSYS_LOG(WARN, "whipping wok failed,ret [%d]", ret);
              }
              else
              {
                black_list.set_wok_send();
              }
            }
          }
        }

      }

      return ret;
    }

    //add wenghaixing [secondary index static_index.exceptional_handle]201504232
    bool ObIndexHandlePool::is_phase_one_need_end()
    {
      bool ret = true;
      for(int64_t i = 0; i < tablet_array_.count(); i++)
      {
        if(-1 != tablet_array_.at(i).if_process_)
        {
          ret = false;
          break;
        }
      }
      return ret;
    }

    bool ObIndexHandlePool::is_phase_two_need_end()
    {
      bool ret = true;
      for(int64_t i = 0; i < range_array_.count(); i++)
      {
        if(-1 != range_array_.at(i).if_process_)
        {
          ret = false;
          break;
        }
      }
      return ret;
    }
    //add e
    int ObIndexHandlePool::push_wok(BlackList &list)
    {
      return black_list_array_.push(list);
    }

    void ObIndexHandlePool::reset()
    {
      tablet_array_.clear();
      range_array_.clear();
      black_list_array_.reset();
      range_have_got_ = 0;
      tablets_have_got_ = 0;
      tablet_index_ = 0;
      range_index_ = 0;
      hist_width_ = 0;
      //schedule_idx_tid_ = OB_INVALID_ID;
      process_idx_tid_ = OB_INVALID_ID;
      total_work_last_end_time_ = 0;
      allocator_.reuse();
    }

    int ObIndexHandlePool::is_tablet_handle(ObTablet *tablet, bool &is_handle)
    {
      int ret = OB_SUCCESS;
      is_handle = false;
      if(NULL == tablet)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        for(int64_t i = 0; i < tablet_array_.count(); i++)
        {
          if(NULL == tablet_array_.at(i).tablet_)
          {
            ret = OB_INVALID_ARGUMENT;
            break;
          }
          else
          {
            if(tablet_array_.at(i).tablet_->get_range() == tablet->get_range())
            {
              is_handle = true;
              break;
            }
          }
        }
      }
      return ret;
    }

    //add wenghaixing[secondary index static_index_build.fix]20150728
    void ObIndexHandlePool::try_stop_mission(uint64_t index_tid)
    {
      //modify by liuxiao [secondary index static_index_build.fix] 20150812
      //bool need_set_time = false;
      //if(0 == total_work_last_end_time_ && index_tid != process_idx_tid_ && index_tid != OB_INVALID_ID)
      if(0 == total_work_last_end_time_ && index_tid != process_idx_tid_)
      //modify e
      {
        pthread_mutex_lock(&mutex_);
        //modify by liuxiao [secondary index static_index_build.fix] 20150818
        if(active_thread_num_ != 0 || (active_thread_num_ == 0 && round_end_ != TABLET_RELEASE))
        //modify e
        {
          TBSYS_LOG(WARN,"try stop index build work current idx_tid:[%ld] new idx_tid:[%ld]",process_idx_tid_,index_tid);
          if(tablet_index_ != tablet_array_.count())
          {
            tablet_index_ = tablet_array_.count();
          }
          if(tablets_have_got_ != tablet_array_.count())
          {
            tablets_have_got_ = tablet_array_.count();
            //delete by liuxiao [secondary index static_index_build.fix] 20150812
            //need_set_time = true;
            //delete e
          }
          if(range_index_ != range_array_.count())
          {
            range_index_ = range_array_.count();
          }
          if(range_have_got_ != range_array_.count())
          {
            range_have_got_ = range_array_.count();
            //delete by liuxiao [secondary index static_index_build.fix] 20150812
            //need_set_time = true;
            //delete e
          }

          //modify by liuxiao [secondary index static_index_build.fix] 20150812
          //if(need_set_time)
          if(true)
          //modify e
          {
            int64_t time = tbsys::CTimeUtil::getTime();
            total_work_last_end_time_ = time;
          }
        }
        pthread_mutex_unlock(&mutex_);
      }
    }
    //add e

  } //end chunkserver

} //end oceanbase
