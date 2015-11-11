#include "ob_sp_procedure.h"
#include "ob_physical_plan.h"

using namespace oceanbase::sql;
using namespace oceanbase::common;

/* ========================================================
 *      SpInst Definition
 * =======================================================*/
int SpInst::serialize_inst(char *buf, int64_t buf_len, int64_t &pos) const
{
  UNUSED(buf);
  UNUSED(buf_len);
  UNUSED(pos);
  TBSYS_LOG(WARN, "Could not serialize inst[%d]", type_);
  return OB_ERROR;
}

int SpInst::deserialize_inst(const char *buf, int64_t data_len, int64_t &pos, common::ModuleArena &allocator, ObPhysicalPlan::OperatorStore &operators_store, ObPhyOperatorFactory *op_factory)
{
  UNUSED(buf);
  UNUSED(data_len);
  UNUSED(pos);
  UNUSED(allocator);
  UNUSED(operators_store);
  UNUSED(op_factory);
  TBSYS_LOG(WARN, "Could not deserialize inst[%d]", type_);
  return OB_ERROR;
}

/* ==============================================
 *    SpExprInst Definition
 * ===============================================*/
int SpExprInst::set_var_val(ObVarAssignVal &var)
{
  var_val_ = var;
  ws_.addVariable(var.variable_name_);
  return OB_SUCCESS;
}

const VariableSet &SpExprInst::get_read_variable_set() const
{
  return var_val_.rs_;
}

const VariableSet &SpExprInst::get_write_variable_set() const
{
  return ws_;
}
int SpExprInst::serialize_inst(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if( OB_SUCCESS != (ret = var_val_.variable_name_.serialize(buf, buf_len, pos)) )
  {
    TBSYS_LOG(WARN, "serialize var_name fail, ret=%d", ret);
  }
  else if( OB_SUCCESS != (ret = var_val_.var_value_->serialize(buf, buf_len, pos)) )
  {
    TBSYS_LOG(WARN, "serialize var_value fail, ret=%d", ret);
  }
  return ret;
}

int SpExprInst::deserialize_inst(const char *buf, int64_t data_len, int64_t &pos, common::ModuleArena &allocator, ObPhysicalPlan::OperatorStore &operators_store, ObPhyOperatorFactory *op_factory)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  UNUSED(operators_store);
  UNUSED(op_factory);
  if( OB_SUCCESS != (ret = var_val_.variable_name_.deserialize(buf, data_len, pos)) )
  {
    TBSYS_LOG(WARN, "deserialize var name fail, ret=%d", ret);
  }
  else
  {
    ObSqlExpression* expr = ObSqlExpression::alloc(); //seems bad design, try to refactor ObVarAssignVal

    if( OB_SUCCESS != (ret = expr->deserialize(buf, data_len, pos)) )
    {
      TBSYS_LOG(WARN, "deserialize expr fail, ret=%d", ret);
    }
    else
    {
      var_val_.var_value_ = expr;
    }
  }
  return ret;
}

/* ===============================================
 *    SpRdBaseInst Definition
 * ==============================================*/
const VariableSet& SpRdBaseInst::get_read_variable_set() const
{
  return rs_;
}

const VariableSet& SpRdBaseInst::get_write_variable_set() const
{
  OB_ASSERT(ws_.var_set_.count() == 0);
  return ws_;
}

void SpRdBaseInst::add_read_var(ObArray<const ObRawExpr*> &var_list)
{
  for(int64_t i = 0; i < var_list.count(); ++i)
  {
    ObItemType raw_type = var_list.at(i)->get_expr_type();
    if( T_SYSTEM_VARIABLE == raw_type || T_TEMP_VARIABLE == raw_type )
    {
      ObString var_name;
      ((const ObConstRawExpr *)var_list.at(i))->get_value().get_varchar(var_name);
      rs_.addVariable(var_name);
//      if( iskey ) rd_base_inst.add_read_var(var_name); //rowkey would be used in read baseline
//      rw_delta_inst.add_read_var(var_name);  //other values are used in update delta
    }
//    add_read_var(var_list.at(i));
  }
}

int SpRdBaseInst::set_rdbase_op(ObPhyOperator *op)
{
  int ret = OB_SUCCESS;
  OB_ASSERT(op->get_type() == PHY_VALUES);
  op_ = op;
  return ret;
}

/* ========================================================
 *      SpRwDeltaInst Definition
 * =======================================================*/
const VariableSet& SpRwDeltaInst::get_read_variable_set() const
{
  return rs_;
}

const VariableSet& SpRwDeltaInst::get_write_variable_set() const
{
//  OB_ASSERT(ws_.var_set_.count() == 0); //since select..into inst inherit this, such assert is not valid any more
  return ws_;
}

void SpRwDeltaInst::add_read_var(ObArray<const ObRawExpr*> &var_list)
{
  for(int64_t i = 0; i < var_list.count(); ++i)
  {
    ObItemType raw_type = var_list.at(i)->get_expr_type();
    if( T_SYSTEM_VARIABLE == raw_type || T_TEMP_VARIABLE == raw_type )
    {
      ObString var_name;
      ((const ObConstRawExpr *)var_list.at(i))->get_value().get_varchar(var_name);
      rs_.addVariable(var_name);
    }
  }
}

int SpRwDeltaInst::set_rwdelta_op(ObPhyOperator *op)
{
  int ret = OB_SUCCESS;
//  OB_ASSERT(op->get_type() == PHY_UPS_EXECUTOR);
  op_ = op;
  return ret;
}

int SpRwDeltaInst::set_ups_exec_op(ObPhyOperator *op)
{
 int ret = OB_SUCCESS;
  OB_ASSERT(op->get_type() == PHY_UPS_EXECUTOR);
  ups_exec_op_ = op;
  return ret;
}

int SpRwDeltaInst::serialize_inst(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
//  ObUpsExecutor *ups_exec = static_cast<ObUpsExecutor *>(op_);
//  ObPhyOperator *ups_main_query = ups_exec->get_inner_plan()->get_main_query();

  if( OB_SUCCESS != (ret = proc_->serialize_tree(buf, buf_len, pos, *op_)) )
  {
    TBSYS_LOG(WARN, "Serialize ups main query fail: ret=%d", ret);
  }
  return ret;
}

int SpRwDeltaInst::deserialize_inst(const char *buf, int64_t data_len, int64_t &pos, common::ModuleArena &allocator, ObPhysicalPlan::OperatorStore &operators_store, ObPhyOperatorFactory *op_factory)
{
  int ret = OB_SUCCESS;
  if( OB_SUCCESS != (ret = proc_->deserialize_tree(buf, data_len, pos, allocator, operators_store, op_factory, op_)) )
  {
    TBSYS_LOG(WARN, "Deserialize rw delta inst query fail: ret=%d", ret);
  }
  return ret;
}

/* ========================================================
 *      SpRwDeltaIntoInst Definition
 * =======================================================*/
int SpRwDeltaIntoVarInst::serialize_inst(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
//  ObUpsExecutor *ups_exec = static_cast<ObUpsExecutor *>(op_);
//  ObPhyOperator *ups_main_query = ups_exec->get_inner_plan()->get_main_query();

  /**
   * serialize the main query[ObUpsModifyWithDmlType] to the ups
   * caution. not the ups_executor
   * */
  if( OB_SUCCESS != (ret = proc_->serialize_tree(buf, buf_len, pos, *op_)) )
  {
    TBSYS_LOG(WARN, "Serialize ups main query fail: ret=%d", ret);
  }
  else if ( OB_SUCCESS != (ret = serialization::encode_i64(buf, buf_len, pos, var_list_.count())) )
  {
    TBSYS_LOG(WARN, "Serialize var list count fail: ret=%d", ret);
  }
  else
  {
    for(int64_t i = 0; i < var_list_.count(); ++i)
    {
      if( OB_SUCCESS != ( ret = var_list_.at(i).serialize(buf, buf_len, pos)) )
      {
        TBSYS_LOG(WARN, "Serialize var list[%ld] fail, ret=%d", i, ret);
        break;
      }
    }
  }
  return ret;
}

int SpRwDeltaIntoVarInst::deserialize_inst(const char *buf, int64_t data_len, int64_t &pos, common::ModuleArena &allocator, ObPhysicalPlan::OperatorStore &operators_store, ObPhyOperatorFactory *op_factory)
{
  int ret = OB_SUCCESS;

  int64_t var_count = 0;
  if( OB_SUCCESS != (ret = proc_->deserialize_tree(buf, data_len, pos, allocator, operators_store, op_factory, op_)))
  {
    TBSYS_LOG(WARN, "Deserialize main query fail: ret=%d", ret);
  }
  else if( OB_SUCCESS != (ret = serialization::decode_i64(buf, data_len, pos, &var_count)))
  {
    TBSYS_LOG(WARN, "Deserialize var count fail: ret=%d", ret);
  }
  else
  {
    var_list_.reserve(var_count);
    for(int64_t i = 0; i < var_count; ++i)
    {
      if( OB_SUCCESS != (ret = var_list_.at(i).deserialize(buf, data_len, pos)) )
      {
        TBSYS_LOG(WARN, "Deserialize var list[%ld] fail, ret=%d", i, ret);
        break;
      }
    }
  }
  return ret;
}

/* ========================================================
 *      SpRwCompInst Definition
 * =======================================================*/


/* ========================================================
 *      SpBlockInsts Definition
 * =======================================================*/
void SpBlockInsts::add_inst(SpInst *inst)
{
  inst_list_.push_back(inst);
  rs_.addVariable(inst->get_read_variable_set());
  ws_.addVariable(inst->get_write_variable_set());
}

int SpBlockInsts::serialize_inst(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  int64_t count = inst_list_.count();

  if( OB_SUCCESS != (ret = serialization::encode_i64(buf, buf_len, pos, count)) )
  {
    TBSYS_LOG(WARN, "serialize inst count fail");
  }

  //serialize instructions
  for(int64_t i = 0; i < count && OB_SUCCESS == ret; ++i)
  {
    SpInst *inst = inst_list_.at(i);
    SpInstType type = inst->get_type();
    switch(type)
    {
    case SP_E_INST:
    case SP_D_INST:
    case SP_DE_INST:
      ret = serialization::encode_i32(buf, buf_len, pos, type);
      inst->serialize_inst(buf, buf_len,pos);
      break;
    default:
      TBSYS_LOG(WARN, "Unsupport serialize inst[%d] to UPS", type);
      break;
    }
  }

  //serialize read variables
  int64_t rd_var_count = rs_.var_set_.count();
  if( OB_SUCCESS != (ret = serialization::encode_i64(buf, buf_len, pos, rd_var_count)))
  {
    TBSYS_LOG(WARN, "serialize read variables count fail");
  }
  for(int64_t rd_var_itr = 0; rd_var_itr < rs_.var_set_.count() && OB_SUCCESS == ret; ++rd_var_itr)
  {
    const ObString &var_name = rs_.var_set_.at(rd_var_itr);
    const ObObj *obj;
    proc_->read_variable(var_name, obj);
    if( NULL == obj )
    {
      TBSYS_LOG(WARN, "variable[%.*s] does not found in session", var_name.length(), var_name.ptr());
    }
    else if( OB_SUCCESS != (ret = var_name.serialize(buf, buf_len, pos)) )
    {
      TBSYS_LOG(WARN, "variable name serialization fail");
    }
    else if( OB_SUCCESS != (ret = obj->serialize(buf, buf_len, pos)) )
    {
      TBSYS_LOG(WARN, "variable value serialization fail");
    }
  }
  return ret;
}

int SpBlockInsts::deserialize_inst(const char *buf, int64_t data_len, int64_t &pos, common::ModuleArena& allocator,
                                   ObPhysicalPlan::OperatorStore& operators_store, ObPhyOperatorFactory *op_factory)
{
  int ret = OB_SUCCESS;
  int64_t count = 0;

  if( OB_SUCCESS != (ret = serialization::decode_i64(buf, data_len, pos, &count)) )
  {
    TBSYS_LOG(WARN, "deserialize inst count fail");
  }
  inst_list_.reserve(count);
  for(int64_t i = 0; i < count && OB_SUCCESS == ret; ++i)
  {
    SpInstType type;
    SpInst *inst = NULL;
    int32_t type_int_value = 0;
    serialization::decode_i32(buf, data_len, pos, &type_int_value);
    type = static_cast<SpInstType>(type_int_value);
    switch (type) {
    case SP_E_INST:
      inst = proc_->create_inst<SpExprInst>();
      inst->deserialize_inst(buf, data_len, pos, allocator, operators_store, op_factory);
      break;
    case SP_D_INST:
      inst = proc_->create_inst<SpRwDeltaInst>();
      inst->deserialize_inst(buf, data_len, pos, allocator, operators_store, op_factory);
      break;
    case SP_DE_INST:
      inst = proc_->create_inst<SpRwDeltaIntoVarInst>();
      inst->deserialize_inst(buf, data_len, pos, allocator, operators_store, op_factory);
      break;
    default:
      TBSYS_LOG(WARN, "Unsupport deserialize inst[%d]", type);
      ret = OB_ERROR;
      break;
    }
    if( OB_SUCCESS == ret )
      add_inst(inst);
    else
      break;
  }

  //Varialbles are saved in ObSqlSessionInfo on ms
  // 	and saved in ObProcedure on ups
  int64_t rd_var_count = 0;
  serialization::decode_i64(buf, data_len, pos, &rd_var_count);

  for(int64_t i = 0; i < rd_var_count && OB_SUCCESS == ret; ++i)
  {
    ObString var_name;
    ObObj obj;
    var_name.deserialize(buf, data_len, pos);
    obj.deserialize(buf, data_len, pos);

    proc_->write_variable(var_name, obj);
  }
  return ret;
}
/*=================================================
 * 					SpProcedure Defintion
 * ===============================================*/
SpProcedure::SpProcedure(){}

SpProcedure::~SpProcedure()
{
  for(int64_t i = 0; i < inst_list_.count(); ++i)
  {
    inst_list_.at(i)->~SpInst();
  }
  arena_.free();
}

int SpProcedure::write_variable(const ObString &var_name, const ObObj &val)
{
  UNUSED(var_name);
  UNUSED(val);
  return OB_SUCCESS;
}

int SpProcedure::read_variable(const ObString &var_name, const ObObj *&val) const
{
  UNUSED(var_name);
  UNUSED(val);
  return OB_SUCCESS;
}

int SpProcedure::read_variable(const ObString &var_name, ObObj &val) const
{
  UNUSED(var_name);
  UNUSED(val);
  return OB_SUCCESS;
}

int SpProcedure::debug_status(const SpInst *inst) const
{
  int ret = OB_SUCCESS;

  if( inst != NULL && inst->get_ownner() == this )
  {
    const VariableSet &rs = inst->get_read_variable_set();
    const VariableSet &ws = inst->get_write_variable_set();
    char debug_buf[1024];
    int64_t buf_len = 1024, pos = 0;

    databuff_printf(debug_buf, buf_len, pos, "inst %ld\n", pc_);

    for(int64_t i = 0; i < rs.var_set_.count(); ++i)
    {
      ObObj val;
      const ObString &var_name = rs.var_set_.at(i);
      if( OB_SUCCESS == read_variable(var_name, val))
      {
        databuff_printf(debug_buf, buf_len, pos, "\tvar [%.*s] = %s\n",
                        var_name.length(), var_name.ptr(), to_cstring(val));
      }
      else
      {
         databuff_printf(debug_buf, buf_len, pos, "\tvar [%.*s] not found.\n",
                         var_name.length(), var_name.ptr());
      }
    }

    for(int64_t i = 0; i < ws.var_set_.count(); ++i)
    {
      ObObj val;
      const ObString &var_name = ws.var_set_.at(i);
      if( OB_SUCCESS == read_variable(var_name, val))
      {
         databuff_printf(debug_buf, buf_len, pos, "\tvar [%.*s] = %s\n",
                         var_name.length(), var_name.ptr(), to_cstring(val));
      }
      else
      {
         databuff_printf(debug_buf, buf_len, pos, "\tvar [%.*s] not found.\n",
                         var_name.length(), var_name.ptr());
      }
    }
    TBSYS_LOG(INFO, "%s", debug_buf);
  }
  return ret;
}

int SpProcedure::serialize_tree(char *buf, int64_t buf_len, int64_t &pos, const ObPhyOperator &root) const
{
  int ret = OB_SUCCESS;

  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = serialization::encode_vi32(buf, buf_len, pos, root.get_type())))
    {
      TBSYS_LOG(WARN, "fail to encode op type:ret[%d]", ret);
    }
    else if (OB_SUCCESS != (ret = root.serialize(buf, buf_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to serialize root:ret[%d] type=%d op=%s", ret, root.get_type(), to_cstring(root));
    }
    else
    {
      TBSYS_LOG(DEBUG, "serialize operator succ, type=%d", root.get_type());
    }
  }

  for (int64_t i=0;OB_SUCCESS == ret && i<root.get_child_num();i++)
  {
    if (NULL != root.get_child(static_cast<int32_t>(i)) )
    {
      if (OB_SUCCESS != (ret = serialize_tree(buf, buf_len, pos, *(root.get_child(static_cast<int32_t>(i))))))
      {
        TBSYS_LOG(WARN, "fail to serialize tree:ret[%d]", ret);
      }
    }
    else
    {
      ret = OB_ERR_UNEXPECTED;
      TBSYS_LOG(WARN, "this operator should has child:type[%d]", root.get_type());
    }
  }
  return ret;
}

int SpProcedure::deserialize_tree(const char *buf, int64_t data_len, int64_t &pos, ModuleArena &allocator,
                                     ObPhysicalPlan::OperatorStore &operators_store, ObPhyOperatorFactory *op_factory, ObPhyOperator *&root)
{
  int ret = OB_SUCCESS;
  int32_t phy_operator_type = 0;
  if (NULL == op_factory)
  {
    ret = OB_NOT_INIT;
    TBSYS_LOG(ERROR, "op_factory == NULL");
  }
  else if (OB_SUCCESS != (ret = serialization::decode_vi32(buf, data_len, pos, &phy_operator_type)))
  {
    TBSYS_LOG(WARN, "fail to decode phy operator type:ret[%d]", ret);
  }

  if (OB_SUCCESS == ret)
  {
    root = op_factory->get_one(static_cast<ObPhyOperatorType>(phy_operator_type), allocator);
    if (NULL == root)
    {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      TBSYS_LOG(WARN, "get operator fail:type[%d]", phy_operator_type);
    }
  }
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = root->deserialize(buf, data_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to deserialize operator:ret[%d]", ret);
    }
    else if (OB_SUCCESS != (ret = operators_store.push_back(root)))
    {
      TBSYS_LOG(WARN, "fail to push operator to operators_store:ret[%d]", ret);
    }
  }

  if (OB_SUCCESS == ret)
  {
    if (root->get_type() <= PHY_INVALID || root->get_type() >= PHY_END)
    {
      ret = OB_ERR_UNEXPECTED;
      TBSYS_LOG(WARN, "invalid operator type:[%d]", root->get_type());
    }
  }

  if (OB_SUCCESS == ret)
  {
    for (int32_t i=0; OB_SUCCESS == ret && i<root->get_child_num(); i++)
    {
      ObPhyOperator *child = NULL;
      if (OB_SUCCESS != (ret = deserialize_tree(buf, data_len, pos, allocator, operators_store, op_factory, child)))
      {
        TBSYS_LOG(WARN, "fail to deserialize tree:ret[%d]", ret);
      }
      else if (OB_SUCCESS != (ret = root->set_child(i, *child)))
      {
        TBSYS_LOG(WARN, "fail to set child:ret[%d]", ret);
      }
    }
  }
  return ret;
}

DEFINE_SERIALIZE(SpProcedure)
{
  //must be only one block inst
  int ret = OB_SUCCESS;
  if( inst_list_.at(0)->get_type() != SP_BLOCK_INST )
  {
    TBSYS_LOG(WARN, "unexpected ups procedure execution");
  }
  else
  {
    inst_list_.at(0)->serialize_inst(buf, buf_len, pos);
  }
  return ret;
}

DEFINE_DESERIALIZE(SpProcedure)
{
  //must be only one block inst
  int ret = OB_SUCCESS;
  SpBlockInsts* block_inst = create_inst<SpBlockInsts>();
  if( OB_SUCCESS != (ret = block_inst->deserialize_inst(
                       buf, data_len, pos, *my_phy_plan_->allocator_,
                       my_phy_plan_->operators_store_,  my_phy_plan_->op_factory_)) )
  {
    TBSYS_LOG(WARN, "deserialize instruction fail");
  }
  return ret;
}

DEFINE_GET_SERIALIZE_SIZE(SpProcedure)
{
  OB_ASSERT(0);
  TBSYS_LOG(WARN, "do not get here, i do not implement");
  return 0;
}

int64_t SpProcedure::to_string(char* buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "Procedure\n");
  for(int64_t i = 0; i < inst_list_.count(); ++i)
  {
    SpInst *inst = inst_list_.at(i);
    databuff_printf(buf, buf_len, pos, "inst %ld: ", i);
    pos += inst->to_string(buf + pos, buf_len -pos);
  }
  return pos;
}

/*=======================================================================
 * 			to_string methods
 * =====================================================================*/
int64_t SpExprInst::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type [E], ws [%.*s], rs [", var_val_.variable_name_.length(), var_val_.variable_name_.ptr());
  const ObArray<ObString> &rs = var_val_.rs_.var_set_;
  for(int64_t i = 0; i < rs.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", rs.at(i).length(), rs.at(i).ptr(), ((i == rs.count()-1) ? ']' : ','));
  }
  if( rs.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");
  databuff_printf(buf, buf_len, pos, "\n");
  return pos;
}

int64_t SpRdBaseInst::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type [B], ws [], rs [");
  const ObArray<ObString> &rs = get_read_variable_set().var_set_;
  for(int64_t i = 0; i < rs.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", rs.at(i).length(), rs.at(i).ptr(), ((i == rs.count()-1) ? ']' : ','));
  }
  if( rs.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");
  databuff_printf(buf, buf_len, pos, ", tid[%ld]\n", table_id_);
  return pos;
}

int64_t SpRwDeltaInst::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type [D], ws [], rs [");
  const ObArray<ObString> &rs = get_read_variable_set().var_set_;
  for(int64_t i = 0; i < rs.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", rs.at(i).length(), rs.at(i).ptr(), ((i == rs.count()-1) ? ']' : ','));
  }
  if( rs.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");
  databuff_printf(buf, buf_len, pos, ", tid[%ld]\n", table_id_);
  return pos;
}


int64_t SpRwCompInst::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type [A], ws [");

  const ObArray<ObString> &ws = get_write_variable_set().var_set_;
  for(int64_t i = 0; i < ws.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", ws.at(i).length(), ws.at(i).ptr(), ((i == ws.count()-1) ? ']' : ','));
  }
  if ( ws.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");

  databuff_printf(buf, buf_len, pos, ", rs [");

  const ObArray<ObString> &rs = get_read_variable_set().var_set_;
  for(int64_t i = 0; i < rs.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", rs.at(i).length(), rs.at(i).ptr(), ((i == rs.count()-1) ? ']' : ','));
  }
  if( rs.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");
  databuff_printf(buf, buf_len, pos, ", tid[%ld]\n", table_id_);
  return pos;
}

int64_t SpRwDeltaIntoVarInst::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type [D], ws [");

  const ObArray<ObString> &ws = get_write_variable_set().var_set_;
  for(int64_t i = 0; i < ws.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", ws.at(i).length(), ws.at(i).ptr(), ((i == ws.count()-1) ? ']' : ','));
  }
  if ( ws.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");

  databuff_printf(buf, buf_len, pos, ", rs [");

  const ObArray<ObString> &rs = get_read_variable_set().var_set_;
  for(int64_t i = 0; i < rs.count(); ++i)
  {
    databuff_printf(buf, buf_len, pos, " %.*s%c", rs.at(i).length(), rs.at(i).ptr(), ((i == rs.count()-1) ? ']' : ','));
  }
  if( rs.count() == 0 )
    databuff_printf(buf, buf_len, pos, "]");
  databuff_printf(buf, buf_len, pos, ", tid[%ld]\n", table_id_);
  return pos;
}

int64_t SpBlockInsts::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "type[Block]\n");
  for(int64_t i = 0; i < inst_list_.count(); ++i)
  {
    SpInst *inst = inst_list_.at(i);
    databuff_printf(buf, buf_len, pos, "\t sub-inst %ld: ", i);
    pos += inst->to_string(buf + pos, buf_len -pos);
  }
  return pos;
}

