/*
  Copyright (c) 2013, Dan VerWeire<dverweire@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <time.h>
#include <uv.h>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_result.h"
#include "odbc_statement.h"

using namespace v8;
using namespace node;

Persistent<FunctionTemplate> ODBCStatement::constructor_template;

void ODBCStatement::Init(v8::Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  // Constructor Template
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->SetClassName(String::NewSymbol("ODBCStatement"));

  // Reserve space for one Handle<Value>
  Local<ObjectTemplate> instance_template = constructor_template->InstanceTemplate();
  instance_template->SetInternalFieldCount(1);
  
  // Prototype Methods
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "execute", Execute);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeDirect", ExecuteDirect);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "prepare", Prepare);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "bind", Bind);

  // Attach the Database Constructor to the target object
  target->Set( v8::String::NewSymbol("ODBCStatement"),
               constructor_template->GetFunction());
  
  scope.Close(Undefined());
}

ODBCStatement::~ODBCStatement() {
  this->Free();
}

void ODBCStatement::Free() {
  if (m_hSTMT) {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeHandle(SQL_HANDLE_STMT, m_hSTMT);
    m_hSTMT = NULL;
    
    uv_mutex_unlock(&ODBC::g_odbcMutex);
    
    if (bufferLength > 0) {
      free(buffer);
    }
  }
}

Handle<Value> ODBCStatement::New(const Arguments& args) {
  HandleScope scope;
  
  REQ_EXT_ARG(0, js_henv);
  REQ_EXT_ARG(1, js_hdbc);
  REQ_EXT_ARG(2, js_hstmt);
  
  HENV hENV = static_cast<HENV>(js_henv->Value());
  HDBC hDBC = static_cast<HDBC>(js_hdbc->Value());
  HSTMT hSTMT = static_cast<HSTMT>(js_hstmt->Value());
  
  //create a new OBCResult object
  ODBCStatement* objODBCResult = new ODBCStatement(hENV, hDBC, hSTMT);
  
  //specify the buffer length
  objODBCResult->bufferLength = MAX_VALUE_SIZE - 1;
  
  //initialze a buffer for this object
  objODBCResult->buffer = (uint16_t *) malloc(objODBCResult->bufferLength + 1);
  //TODO: make sure the malloc succeeded

  //set the initial colCount to 0
  objODBCResult->colCount = 0;
  
  objODBCResult->Wrap(args.Holder());
  
  return scope.Close(args.Holder());
}

/*
 * Execute
 * 
 */

Handle<Value> ODBCStatement::Execute(const Arguments& args) {
  DEBUG_PRINTF("ODBCStatement::Execute\n");
  
  HandleScope scope;

  REQ_FUN_ARG(0, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  execute_work_data* data = 
    (execute_work_data *) calloc(1, sizeof(execute_work_data));

  data->cb = Persistent<Function>::New(cb);
  
  data->stmt = stmt;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(),
    work_req,
    UV_Execute,
    (uv_after_work_cb)UV_AfterExecute);

  stmt->Ref();

  return  scope.Close(Undefined());
}

void ODBCStatement::UV_Execute(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Execute\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLExecute(data->stmt->m_hSTMT); 

  data->result = ret;
}

void ODBCStatement::UV_AfterExecute(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterExecute\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);
  
  HandleScope scope;
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      self->m_hENV,
      self->m_hDBC,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[3];
    args[0] = External::New(self->m_hENV);
    args[1] = External::New(self->m_hDBC);
    args[2] = External::New(self->m_hSTMT);
    
    Persistent<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(3, args));

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Object>::New(js_result);
    
    data->cb->Call(Context::GetCurrent()->Global(), 2, args);
  }
  
  TryCatch try_catch;
  
  self->Unref();
  
  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
  
  data->cb.Dispose();
  
  free(data);
  free(req);
  
  scope.Close(Undefined());
}

/*
 * ExecuteDirect
 * 
 */

Handle<Value> ODBCStatement::ExecuteDirect(const Arguments& args) {
  DEBUG_PRINTF("ODBCStatement::ExecuteDirect\n");
  
  HandleScope scope;

  REQ_STR_ARG(0, sql);
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  execute_direct_work_data* data = 
    (execute_direct_work_data *) calloc(1, sizeof(execute_direct_work_data));

  data->sql = (char *) malloc(sql.length() +1);
  data->cb = Persistent<Function>::New(cb);
  
  strcpy(data->sql, *sql);
  
  data->stmt = stmt;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(),
    work_req, 
    UV_ExecuteDirect, 
    (uv_after_work_cb)UV_AfterExecuteDirect);

  stmt->Ref();

  return  scope.Close(Undefined());
}

void ODBCStatement::UV_ExecuteDirect(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_ExecuteDirect\n");
  
  execute_direct_work_data* data = (execute_direct_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLExecDirect(
    data->stmt->m_hSTMT,
    (SQLCHAR *) data->sql, 
    strlen(data->sql));  

  data->result = ret;
}

void ODBCStatement::UV_AfterExecuteDirect(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterExecuteDirect\n");
  
  execute_direct_work_data* data = (execute_direct_work_data *)(req->data);
  
  HandleScope scope;
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      self->m_hENV,
      self->m_hDBC,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[3];
    args[0] = External::New(self->m_hENV);
    args[1] = External::New(self->m_hDBC);
    args[2] = External::New(self->m_hSTMT);
    
    Persistent<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(3, args));

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Object>::New(js_result);
    
    data->cb->Call(Context::GetCurrent()->Global(), 2, args);
  }
  
  TryCatch try_catch;
  
  self->Unref();
  
  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
  
  data->cb.Dispose();
  
  free(data->sql);
  free(data);
  free(req);
  
  scope.Close(Undefined());
}

/*
 * Prepare
 * 
 */

Handle<Value> ODBCStatement::Prepare(const Arguments& args) {
  DEBUG_PRINTF("ODBCStatement::Prepare\n");
  
  HandleScope scope;

  REQ_STR_ARG(0, sql);
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  prepare_work_data* data = 
    (prepare_work_data *) calloc(1, sizeof(prepare_work_data));

  data->sql = (char *) malloc(sql.length() +1);
  data->cb = Persistent<Function>::New(cb);
  
  strcpy(data->sql, *sql);
  
  data->stmt = stmt;
  
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_Prepare, 
    (uv_after_work_cb)UV_AfterPrepare);

  stmt->Ref();

  return  scope.Close(Undefined());
}

void ODBCStatement::UV_Prepare(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Prepare\n");
  
  prepare_work_data* data = (prepare_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLPrepare(
    data->stmt->m_hSTMT,
    (SQLCHAR *) data->sql, 
    strlen(data->sql));

  data->result = ret;
}

void ODBCStatement::UV_AfterPrepare(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterPrepare\n");
  
  prepare_work_data* data = (prepare_work_data *)(req->data);
  
  HandleScope scope;
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
     ODBC::CallbackSQLError(
      self->m_hENV,
      self->m_hDBC,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[2];

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Value>::New(True());
    
    data->cb->Call(Context::GetCurrent()->Global(), 2, args);
  }
  
  TryCatch try_catch;
  
  self->Unref();
  
  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
  
  data->cb.Dispose();
  
  free(data->sql);
  free(data);
  free(req);
  
  scope.Close(Undefined());
}

/*
 * Bind
 * 
 */

Handle<Value> ODBCStatement::Bind(const Arguments& args) {
  DEBUG_PRINTF("ODBCStatement::Bind\n");
  
  HandleScope scope;

  if ( !args[0]->IsArray() ) {
    return ThrowException(Exception::TypeError(
              String::New("Argument 1 must be an Array"))
    );
  }
  
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  bind_work_data* data = 
    (bind_work_data *) calloc(1, sizeof(bind_work_data));

  data->stmt = stmt;
  
  data->cb = Persistent<Function>::New(cb);
  
  data->params = ODBC::GetParametersFromArray(
    Local<Array>::Cast(args[0]), 
    &data->paramCount);
  
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_Bind, 
    (uv_after_work_cb)UV_AfterBind);

  stmt->Ref();

  return  scope.Close(Undefined());
}

void ODBCStatement::UV_Bind(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Bind\n");
  
  bind_work_data* data = (bind_work_data *)(req->data);

  SQLRETURN ret;
  Parameter prm;
  
  for (int i = 0; i < data->paramCount; i++) {
    prm = data->params[i];
    
    DEBUG_PRINTF(
      "ODBC::UV_Bind - param[%i]: c_type=%i type=%i "
      "buffer_length=%i size=%i length=%i &length=%X decimals=%i\n",
      i, prm.c_type, prm.type, prm.buffer_length, prm.size, prm.length, 
      &data->params[i].length, prm.decimals
    );

    ret = SQLBindParameter(
      data->stmt->m_hSTMT,        //StatementHandle
      i + 1,                      //ParameterNumber
      SQL_PARAM_INPUT,            //InputOutputType
      prm.c_type,                 //ValueType
      prm.type,                   //ParameterType
      prm.size,                   //ColumnSize
      prm.decimals,               //DecimalDigits
      prm.buffer,                 //ParameterValuePtr
      prm.buffer_length,          //BufferLength
      //using &prm.length did not work here...
      &data->params[i].length);   //StrLen_or_IndPtr

    if (ret == SQL_ERROR) {
      break;
    }
  }

  data->result = ret;
  
  //free memory
  for (int i = 0; i < data->paramCount; i++) {
    if (prm = data->params[i], prm.buffer != NULL) {
      switch (prm.c_type) {
        case SQL_C_CHAR:    free(prm.buffer);             break; 
        case SQL_C_SBIGINT: delete (int64_t *)prm.buffer; break;
        case SQL_C_DOUBLE:  delete (double  *)prm.buffer; break;
        case SQL_C_BIT:     delete (bool    *)prm.buffer; break;
      }
    }
  }

  free(data->params);
}

void ODBCStatement::UV_AfterBind(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterBind\n");
  
  bind_work_data* data = (bind_work_data *)(req->data);
  
  HandleScope scope;
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //Check if there were errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      self->m_hENV,
      self->m_hDBC,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[2];

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Value>::New(True());
    
    data->cb->Call(Context::GetCurrent()->Global(), 2, args);
  }
  
  TryCatch try_catch;
  
  self->Unref();
  
  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
  
  data->cb.Dispose();
  
  free(data);
  free(req);
  
  scope.Close(Undefined());
}


/*
void ODBC::UV_Tables(uv_work_t* req) {
  query_request* prep_req = (query_request *)(req->data);
  
  uv_mutex_lock(&ODBC::g_odbcMutex);
  SQLAllocStmt(prep_req->dbo->m_hDBC,&prep_req->hSTMT );
  uv_mutex_unlock(&ODBC::g_odbcMutex);
  
  SQLRETURN ret = SQLTables( 
    prep_req->hSTMT, 
    (SQLCHAR *) prep_req->catalog,   SQL_NTS, 
    (SQLCHAR *) prep_req->schema,   SQL_NTS, 
    (SQLCHAR *) prep_req->table,   SQL_NTS, 
    (SQLCHAR *) prep_req->type,   SQL_NTS
  );
  
  // this will be checked later in UV_AfterQuery
  prep_req->result = ret; 
}

Handle<Value> ODBC::Tables(const Arguments& args) {
  HandleScope scope;

  REQ_STR_OR_NULL_ARG(0, catalog);
  REQ_STR_OR_NULL_ARG(1, schema);
  REQ_STR_OR_NULL_ARG(2, table);
  REQ_STR_OR_NULL_ARG(3, type);
  Local<Function> cb = Local<Function>::Cast(args[4]);

  ODBC* dbo = ObjectWrap::Unwrap<ODBC>(args.Holder());
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  query_request* prep_req = (query_request *) calloc(1, sizeof(query_request));
  
  if (!prep_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  prep_req->sql = NULL;
  prep_req->catalog = NULL;
  prep_req->schema = NULL;
  prep_req->table = NULL;
  prep_req->type = NULL;
  prep_req->column = NULL;
  prep_req->cb = Persistent<Function>::New(cb);

  if (!String::New(*catalog)->Equals(String::New("null"))) {
    prep_req->catalog = (char *) malloc(catalog.length() +1);
    strcpy(prep_req->catalog, *catalog);
  }
  
  if (!String::New(*schema)->Equals(String::New("null"))) {
    prep_req->schema = (char *) malloc(schema.length() +1);
    strcpy(prep_req->schema, *schema);
  }
  
  if (!String::New(*table)->Equals(String::New("null"))) {
    prep_req->table = (char *) malloc(table.length() +1);
    strcpy(prep_req->table, *table);
  }
  
  if (!String::New(*type)->Equals(String::New("null"))) {
    prep_req->type = (char *) malloc(type.length() +1);
    strcpy(prep_req->type, *type);
  }
  
  prep_req->dbo = dbo;
  work_req->data = prep_req;
  
  uv_queue_work(uv_default_loop(), work_req, UV_Tables, (uv_after_work_cb)UV_AfterQueryAll);

  dbo->Ref();

  return scope.Close(Undefined());
}

void ODBC::UV_Columns(uv_work_t* req) {
  query_request* prep_req = (query_request *)(req->data);
  
  SQLAllocStmt(prep_req->dbo->m_hDBC,&prep_req->hSTMT );
  
  SQLRETURN ret = SQLColumns( 
    prep_req->hSTMT, 
    (SQLCHAR *) prep_req->catalog,   SQL_NTS, 
    (SQLCHAR *) prep_req->schema,   SQL_NTS, 
    (SQLCHAR *) prep_req->table,   SQL_NTS, 
    (SQLCHAR *) prep_req->column,   SQL_NTS
  );
  
  // this will be checked later in UV_AfterQuery
  prep_req->result = ret;
}

Handle<Value> ODBC::Columns(const Arguments& args) {
  HandleScope scope;

  REQ_STR_OR_NULL_ARG(0, catalog);
  REQ_STR_OR_NULL_ARG(1, schema);
  REQ_STR_OR_NULL_ARG(2, table);
  REQ_STR_OR_NULL_ARG(3, column);
  Local<Function> cb = Local<Function>::Cast(args[4]);
  
  ODBC* dbo = ObjectWrap::Unwrap<ODBC>(args.Holder());
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  query_request* prep_req = (query_request *) calloc(1, sizeof(query_request));
  
  if (!prep_req) {
    V8::LowMemoryNotification();
    return ThrowException(Exception::Error(String::New("Could not allocate enough memory")));
  }

  prep_req->sql = NULL;
  prep_req->catalog = NULL;
  prep_req->schema = NULL;
  prep_req->table = NULL;
  prep_req->type = NULL;
  prep_req->column = NULL;
  prep_req->cb = Persistent<Function>::New(cb);

  if (!String::New(*catalog)->Equals(String::New("null"))) {
    prep_req->catalog = (char *) malloc(catalog.length() +1);
    strcpy(prep_req->catalog, *catalog);
  }
  
  if (!String::New(*schema)->Equals(String::New("null"))) {
    prep_req->schema = (char *) malloc(schema.length() +1);
    strcpy(prep_req->schema, *schema);
  }
  
  if (!String::New(*table)->Equals(String::New("null"))) {
    prep_req->table = (char *) malloc(table.length() +1);
    strcpy(prep_req->table, *table);
  }
  
  if (!String::New(*column)->Equals(String::New("null"))) {
    prep_req->column = (char *) malloc(column.length() +1);
    strcpy(prep_req->column, *column);
  }
  
  prep_req->dbo = dbo;
  work_req->data = prep_req;
  
  uv_queue_work(uv_default_loop(), work_req, UV_Columns, (uv_after_work_cb)UV_AfterQueryAll);
  
  dbo->Ref();

  return scope.Close(Undefined());
}
*/