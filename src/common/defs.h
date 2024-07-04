#ifndef __DEFS_H__
#define __DEFS_H__

#include <stdlib.h>
#include <stdio.h>
#include "cli_val.h"

#define boolean int

#define LOWEST_PRIORITY 1000

#define MAX_DEPTH 128

#define ENV_ACTION_NAME "COMMIT_ACTION"
#define ENV_ACTION_DELETE "DELETE"
#define ENV_ACTION_SET "SET"
#define ENV_ACTION_ACTIVE "ACTIVE"
#define ENV_SIBLING_POSITION "COMMIT_SIBLING_POSITION"
#define ENV_DATA_PATH "NODE_DATA_PATH"
#define ENV_COMMIT_STATUS "COMMIT_STATUS"

#define COMMIT_CHANGES_FILE "/tmp/.changes"
#define POST_COMMIT_HOOK_DIR "/etc/commit"
#define PRE_COMMIT_HOOK_DIR "/etc/pre_commit"

struct Result
{
  int _err_code;
  char *_err_str;
  int _action;
  int _mode;
  void* _data;
};

typedef enum {
  K_NO_OP = 0x01,
  K_ACTIVE_OP = 0x02, //as a result of an already created node, but assuming action
  K_SET_OP = 0x04,
  K_CREATE_OP = 0x08,
  K_DEL_OP = 0x10
} NODE_OPERATION;

#define IS_SET(op)    (op & K_SET_OP)
#define IS_ACTIVE(op) (op & K_ACTIVE_OP)
#define IS_CREATE(op) (op & K_CREATE_OP)
#define IS_DELETE(op) (op & K_DEL_OP)
#define IS_NOOP(op)   (op & K_NO_OP)
#define IS_SET_OR_CREATE(op) ((op & K_SET_OP) || (op & K_CREATE_OP))

/**
 * keeps both configuration and template data in single structure
 *
 **/

/*
TODO: either port over to new system or retain complete set of cli_val definiaitons.
remove _actions and rely on def in the future.
 */
struct Config
{
  boolean      _multi;
  int          _priority;
  char*        _priority_extended;
  int          _limit;
  vtw_def      _def; //keep this here
  char*        _help;
  char*        _default;
  char*        _path;
};

/*
 * is used to define embedded nodes (multi) and leafs
 */
struct Data
{
  char*           _name;   //name of this node
  boolean         _value;  //is this a value?
  char*           _path;
  NODE_OPERATION  _operation; //no-op, set, or delete
};

/*
 * additional data to be defined and used by client
 */
struct Aux
{
  boolean        _first;
  boolean        _last;
};

struct VyattaNode
{
  struct Data   _data;
  struct Config _config;
  struct Aux    _aux;
};

#endif //__DEFS_H__
