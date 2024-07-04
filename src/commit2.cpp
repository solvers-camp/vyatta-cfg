#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <string.h>
#include <sys/time.h>
#include <glib-object.h>  /* g_type_init */
#include "common/common.h"
#include "cli_path_utils.h"
#include "vyos-errors.h"

#define cond_plog(cond, prio, fmt, ...) do { \
  if (cond) { \
    printf(fmt "\n", ##__VA_ARGS__); \
    syslog(prio, fmt, ##__VA_ARGS__) ;\
  } \
} while (0);

#define dplog(fmt, ...) cond_plog(true, LOG_DEBUG, fmt, ##__VA_ARGS__);
#define d_dplog(fmt, ...) cond_plog(g_debug, LOG_DEBUG, fmt, ##__VA_ARGS__);

boolean g_debug = FALSE;
boolean g_display_error_node = FALSE;
boolean g_coverage = FALSE;
boolean g_dump_trans = FALSE;
boolean g_dump_actions = FALSE;
boolean g_print_error_location_all = FALSE;
boolean g_old_print_output = FALSE;

#define g_num_actions 5

// this uses the vtw_act_type enum
const int ActionOrder[g_num_actions] = {
  begin_act,
  delete_act,
  create_act,
  update_act,
  end_act
};

// this corresponds to the vtw_act_type enum
static const char* ActionNames[top_act] = {
  "delete",   //0
  "create",   //1
  "activate", //2
  "update",   //3
  "syntax",   //4
  "commit",   //5
  "begin",    //6
  "end"       //7
};

GNode*
get_transactions(GNode*, boolean priority);

boolean
complete(GSList *node_coll, boolean test_mode);

gboolean
sort_func(GNode *node, gpointer data, boolean priority_mode);

gboolean
sort_func_priority(GNode *node, gpointer data);

gboolean
sort_func_priority_extended(GNode *node, gpointer data);

gboolean
sort_func_simple(GNode *node, gpointer data);

void
cleanup(GNode *root_node);

gboolean
dump_func(GNode *node, gpointer data);

static gboolean
enclosing_process_func(GNode *node, gpointer data);

static gboolean
process_func(GNode *node, gpointer data);

boolean
process_priority_node(GNode *priority_node);

void
execute_hook(const char* dir, const char* comment);

static gboolean
enclosing_process_func(GNode *node, gpointer data);

static gboolean
validate_func(GNode *node, gpointer data);

static boolean
validate_configuration(GNode *root_node, boolean mode,
                       GSList **nodes_visited_coll);

static void
update_change_file(FILE *fp, GSList *coll);

static gboolean
execute_hook_compare_func(gconstpointer a, gconstpointer b, gpointer data);

static gboolean
execute_hook_func(GNode *node, gpointer data);

char*
process_script_path(char* in);

/*
 * NOTES: reverse: use the n-nary tree in commit2.c and only encapuslate
 * data store. pass in func pointer for processing of commands below.
 *
 * also, the algorithm for collapsing the tree into a transaction list is:
 *   1) iterate through tree and mark all explicit transactions
 *   2) when done, prune the tree of all root explicit transactions
 *   3) Now iterate through remaining tree and remove each node and append
 *      to transaction list.
 */

/**
 *
 **/
void 
usage(void)
{
  printf("commit2\n");
  printf("\t-d\t\tdebug mode\n");
  printf("\t-s\t\tdump sorted transactions and exit\n");
  printf("\t-a\t\tdump ordered node actions and exit\n");
  printf("\t-p\t\tdisable priority mode\n");
  printf("\t-t\t\ttest mode (don't apply directory modifications)\n");
  printf("\t-e\t\tprint node where error occurred\n");
  printf("\t-c\t\tdump node coverage and execution times\n");
  printf("\t-o\t\tdisable partial commit\n");
  printf("\t-f\t\tfull iteration over configuration on commit check\n");
  printf("\t-b [priority]\tbreak at priority node (debug mode)\n");
  printf("\t-r\t\tdisable run hook script on finishing commit\n");
  printf("\t-x\t\tdisable new print feature\n");
  printf("\t-l\t\tforce commit through removal of commit lock\n");
  printf("\t-h\t\thelp\n");
}

/**
 *
 **/
int
main(int argc, char** argv)
{
  int ch;
  boolean priority_mode = TRUE;
  boolean test_mode = FALSE;
  boolean disable_partial_commit = FALSE;
  boolean full_commit_check = FALSE;
  boolean break_priority = FALSE;
  int     break_priority_node = -1;
  boolean disable_hook = FALSE;
  char   *commit_comment  = NULL;  

  /* this is needed before calling certain glib functions */
  #if !GLIB_CHECK_VERSION(2,35,0)
  g_type_init();
  #endif

  //grab inputs
  while ((ch = getopt(argc, argv, "xdpthsecoafb:rlC:")) != -1) {
    switch (ch) {
    case 'x':
      g_old_print_output = TRUE;
      break;
    case 'd':
      g_debug = TRUE;
      break;
    case 'h':
      usage();
      exit(0);
      break;
    case 'p':
      priority_mode = FALSE;
      break;
    case 't':
      test_mode = TRUE;
      break;
    case 's':
      g_dump_trans = TRUE;
      break;
    case 'e':
      g_display_error_node = TRUE;
      break;
    case 'c':
      g_coverage = TRUE;
      break;
    case 'o':
      disable_partial_commit = TRUE;
      break;
    case 'a':
      g_dump_actions = TRUE;
      break;
    case 'f':
      full_commit_check = TRUE;
      break;
    case 'b':
      break_priority_node = strtoul(optarg,NULL,10);
      break_priority = TRUE;
      break;
    case 'r':
      disable_hook = TRUE;
      break;
    case 'C':
      commit_comment = strdup(optarg);
      break;
    case 'l':
      release_config_lock();
      break;
    default:
      usage();
      exit(0);
    }
  }

  //can also be set via environment variable
  if (getenv("VYATTA_OUTPUT_ERROR_LOCATION") != NULL) {
    g_print_error_location_all = TRUE;
  }

  if (disable_hook == FALSE) {
    execute_hook(PRE_COMMIT_HOOK_DIR,commit_comment);
  } 

  initialize_output("Commit");
  init_paths(TRUE);
  d_dplog("commit2: starting up");

  if (get_config_lock() != 0) {
    fprintf(out_stream, "Configuration system temporarily locked "
                        "due to another commit in progress\n");
    exit(1);
  }

  //get local session data plus configuration data
  GNode *config_data = common_get_local_session_data();
  if (g_node_n_children(config_data) == 0) {
    common_commit_clean_temp_config(NULL, test_mode);
    fprintf(out_stream, "No configuration changes to commit\n");
    return 0;
  }

  GNode *orig_node_tree = g_node_copy(config_data);

  // Get collection of transactions, i.e. trans nodes that have been activated. 
  GNode *trans_coll = get_transactions(config_data, priority_mode);
  if (trans_coll == NULL) {
    printf("commit2: transactions collection is empty, exiting\n");
    exit(0);
  }

  if (g_debug == TRUE || g_dump_trans == TRUE) {
    if (g_dump_trans == TRUE) {
      fprintf(out_stream,"Dumping transactions\n");
      syslog(LOG_DEBUG,"Dumping transactions");
    }
    //iterate over config_data and dump...
    g_node_traverse(trans_coll,
                    G_PRE_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)dump_func,
                    (gpointer)NULL);
    if (g_dump_trans == TRUE) {
      exit(0);
    }
  }

  set_in_commit(TRUE);

  GNode *trans_child_node = (GNode*)g_node_first_child(trans_coll);
  if (trans_child_node == NULL) {
    printf("commit2: No child nodes to process, exiting\n");
    exit(0);
  }

  //open the changes file and clear
  FILE *fp_changes = fopen(COMMIT_CHANGES_FILE,"w");
  if (fp_changes == NULL) {
    cond_plog(true, LOG_ERR, "commit2: Cannot access changes file, exiting");
    exit(0);
  }

  GSList *completed_root_node_coll = NULL;
  GSList *nodes_visited_coll = NULL;
  int errors = 0;
  int i = 0;
  do {
    boolean success = FALSE;
    d_dplog("commit2: Starting new transaction processing pass on root: [%s]",
            ((trans_child_node && trans_child_node->data
              && ((struct VyattaNode*)(trans_child_node->data))->_data._name)
             ? ((struct VyattaNode*)(trans_child_node->data))->_data._name
               : ""));

    if (break_priority) {
      gpointer gp = ((GNode*)trans_child_node)->data;
      long p = (long) ((struct VyattaNode*)gp)->_config._priority;
      if (p >= break_priority_node) {
        g_dump_trans = TRUE;
        g_node_traverse(trans_child_node,
                        G_PRE_ORDER,
                        G_TRAVERSE_ALL,
                        -1,
                        (GNodeTraverseFunc)dump_func,
                        (gpointer)NULL);
        g_dump_trans = FALSE;
        fprintf(out_stream,"Press any key to commit...\n");
        
        // Wait for single character 
        char input = getchar(); 
        input = input; //to fix stupid compilier warning
      }
    }

    //complete() now requires a undisturbed copy of the trans_child_node tree
    GNode *comp_cp_node = g_node_copy(trans_child_node);

    if (g_dump_actions == TRUE) {
      fprintf(out_stream,"\n"); //add an extra line here
    }

    //on each priority node now execute actions

    nodes_visited_coll = NULL;
    if (validate_configuration(trans_child_node, full_commit_check,
                               &nodes_visited_coll) == TRUE
        && (success = process_priority_node(trans_child_node)) == TRUE) {
      //this below copies the node directory from the local to active location
      //if this is true root skip
      if (trans_child_node != NULL && trans_child_node->data != NULL
          && strcmp(((struct VyattaNode*)(trans_child_node->data))
                    ->_data._path, "/") == 0) {
        //no op, need better way to define true root
      }
      else {
        if (disable_partial_commit == FALSE && g_dump_actions == FALSE) {
          completed_root_node_coll
            = g_slist_append(completed_root_node_coll, comp_cp_node);
        }
      }
    }

    if (g_dump_actions == TRUE) {
      success = TRUE; //FORCE SUCCESS ON DISPLAY MODE OF ACTIONS
    }

    if (success == FALSE) {
      errors |= 1;
      d_dplog("commit2: Failed in processing node");
    }
    else {
      errors |= 2;
    }

    //now update the changes file
    update_change_file(fp_changes,nodes_visited_coll);
    fflush(fp_changes);

    ++i;
  } while ((trans_child_node
              = (GNode*) g_node_nth_child((GNode*) trans_coll,
                                          (guint) i)) != NULL);

  if (errors == 2) {
    /*
     * Need to add to the following func below to clean up dangling .wh. files
     */
    if (g_dump_actions == FALSE) {
      common_commit_copy_to_live_config(orig_node_tree, TRUE, test_mode);
      common_commit_clean_temp_config(orig_node_tree, test_mode);
    }
    d_dplog("commit2: successful commit, now cleaning up temp directories");
  }
  else {
    fprintf(out_stream,"Commit failed\n");
    complete(completed_root_node_coll, test_mode);
  }

  set_in_commit(FALSE);
  d_dplog("DONE");
  
  if (fp_changes != NULL) {
    fclose(fp_changes);
  }
 
  restore_output();
  if (disable_hook == FALSE) {
    if (errors == 2) {
      setenv(ENV_COMMIT_STATUS,"SUCCESS",1);
    }
    else if (errors == 3) {
      setenv(ENV_COMMIT_STATUS,"PARTIAL",1);
    }
    else {
      setenv(ENV_COMMIT_STATUS,"FAILURE",1);
    }
    execute_hook(POST_COMMIT_HOOK_DIR,commit_comment);
    unsetenv(ENV_COMMIT_STATUS);
  } 

  //remove tmp changes file as all the work is now done
  unlink(COMMIT_CHANGES_FILE);

  int vyos_exit_code;
  if (errors == 2) {
    vyos_exit_code = 0;
  }
  else if (errors == 3) {
    vyos_exit_code = VYOS_PARTIAL_COMMIT;
  }
  else {
    vyos_exit_code = VYOS_COMMIT_FAILURE;
  }

  exit (vyos_exit_code);
}

struct ExecuteHookData
{
  char *_dir;
  char *_comment;
};

/**
 *
 **/
void
execute_hook(const char* dir, const char* comment)
{
  if (dir == NULL) {
    return;
  }

  GPtrArray *gparray;
  gparray = g_ptr_array_new();
  DIR *dp;
  if ((dp = opendir(dir)) == NULL){
    d_dplog("could not open hook directory");
    return;
  }

  struct dirent *dirp = NULL;
  while ((dirp = readdir(dp)) != NULL) {
    if (strcmp(dirp->d_name, ".") != 0 && 
        strcmp(dirp->d_name, "..") != 0) {
      
      char *dirname = (char*)malloc(strlen(dirp->d_name)+1);
      strcpy(dirname,dirp->d_name);
      g_ptr_array_add (gparray, (gpointer) dirname);
    }
  }    
  //sort gparray here
  g_ptr_array_sort(gparray,(GCompareFunc)execute_hook_compare_func);
  
  struct ExecuteHookData ehd;
  ehd._comment = (char*)comment;
  ehd._dir = (char*)dir;
  
  //now loop over gparray here
  g_ptr_array_foreach(gparray,(GFunc)execute_hook_func,(gpointer)&ehd);

  g_ptr_array_free (gparray, TRUE);
  closedir(dp);
}

/**
 *
 **/
gboolean
execute_hook_compare_func(gconstpointer a, gconstpointer b, gpointer data)
{
  //just compare first two chars numerically
  char* c_a = *(char**)a;
  char* c_b = *(char**)b;
  int i_a = strtoul(c_a,NULL,10);
  int i_b = strtoul(c_b,NULL,10);
  return (i_a >= i_b);
}

/**
 *
 **/  
gboolean
execute_hook_func(GNode *node, gpointer data)
{
  const char *comment = ((struct ExecuteHookData*)data)->_comment;
  char *dir = ((struct ExecuteHookData*)data)->_dir;
  char *name = (char*)node;

  if (name == NULL || dir == NULL) {
    return FALSE;
  }
  
  char buf[MAX_LENGTH_DIR_PATH*sizeof(char)];
  if (comment == NULL) {
    comment="commit";
  }
  
  sprintf(buf,"%s/%s %s",dir,name, comment);
  syslog(LOG_DEBUG,"Starting commit hook: %s",buf);
  
  if (system(buf) == -1) {
    syslog(LOG_WARNING,"Error on call to hook: %s", buf);
  }
  syslog(LOG_DEBUG,"Finished with commit hook: %s",buf);

  //release directory name
  free(name);

  return FALSE;
}

/**
 *
 **/
static gboolean
process_func(GNode *node, gpointer data)
{
  if (node == NULL) {
    return TRUE;
  }

  struct Result *result = (struct Result*)data;
  gpointer gp = ((GNode*)node)->data;
  struct Config *c = &((struct VyattaNode*)gp)->_config;
  struct Data *d = &((struct VyattaNode*)gp)->_data;
  struct Aux *a = &((struct VyattaNode*)gp)->_aux;
  NODE_OPERATION op = d->_operation;

  int status = 0;
  if (c->_def.actions  && 
      c->_def.actions[result->_action].vtw_list_head){
    d_dplog("commit2::process_func(), calling process on : %s for action "
            "%d, type: %d, operation: %d, path: %s",
            (d->_name ? d->_name : "[n/a]"), result->_action,
            (d->_name ? c->_def.def_type : -1), op, d->_path);

    /* Needs to be cleaned up a bit such that this convoluted if clause
     * is easier to read.
     * (XXX original comment no longer correct and therefore is removed.)
     */
    if ((IS_SET(op) && !IS_ACTIVE(op)
         && (result->_action != delete_act && result->_action != create_act))
        ||
        (IS_CREATE(op) && !IS_ACTIVE(op)
         && (result->_action == begin_act || result->_action == end_act
             || result->_action == create_act
             || (result->_action == update_act
                 && !c->_def.actions[create_act].vtw_list_head)))
        ||
        (IS_ACTIVE(op) && ((result->_action == begin_act)
                           || (result->_action == end_act)))
        ||
        (IS_DELETE(op) && ((result->_action == delete_act)
                           || (result->_action == begin_act)
                           || (result->_action == end_act)))) {
      //NEED TO ADD IF CREATE, THEN CREATE OR UPDATE
      //IF SET THEN UPDATE

      /* let's skip the case where this is active and it's a
       * delete--shouldn't be done, but needs to be include in the rule
       * set above
       */
      if (IS_DELETE(op) && IS_ACTIVE(op) && result->_action == delete_act) {
        return FALSE;
      }

      /* let's skip any multi-node that does not have have a value
       * (an empty multi-node)
       */
      if (c->_multi && node->children == NULL) {
        return FALSE;
      }

      //look at parent for multi tag
      if (d->_value && d->_name) {
        char *val = d->_name;
        if (c->_def.tag) {
          /* need to handle the embedded multinode as a special
           * case--should be fixed!
           */
          val = (char*)clind_unescape(d->_name);
        }
        d_dplog("commit2::process_func(): @ value: %s", (char *) val);
        set_at_string(val); //embedded multinode value
      }
      else {
        if (g_debug) {
          dplog("commit2::process_func(): boolean value is: %d", d->_value);
          if (node->parent != NULL
              && ((struct VyattaNode*)(node->parent->data))->_data._name
                 != NULL) {
            dplog("commit2::process_func(): parent has a name: %s",
                  ((struct VyattaNode*)(node->parent->data))->_data._name);
          }
          dplog("commit2::process_func(): @ value: [NULL]");
        }
      }
      
      common_set_context(c->_path,d->_path);
      d_dplog("Executing %s on this node", ActionNames[result->_action]);

      if (g_coverage) {
        struct timeval t;
        gettimeofday(&t,NULL);
        fprintf(out_stream, "[START] %lu:%lu, %s@%s",
                (unsigned long) t.tv_sec, (unsigned long) t.tv_usec,
                ActionNames[result->_action], d->_path);
      }

      if (result->_action == delete_act) {
        set_in_delete_action(TRUE);
      }

      //set location env
      setenv(ENV_DATA_PATH,d->_path,1);

      if (a->_first && a->_last) {
        setenv(ENV_SIBLING_POSITION,"FIRSTLAST",1);
      }
      else if (a->_first) {
        setenv(ENV_SIBLING_POSITION,"FIRST",1);
      }
      else if (a->_last) {
        setenv(ENV_SIBLING_POSITION,"LAST",1);
      }

      //do not set for promoted actions
      if (!IS_ACTIVE(op)) {
        if (IS_DELETE(op)) {
            setenv(ENV_ACTION_NAME,ENV_ACTION_DELETE,1);
        }
        else {
          setenv(ENV_ACTION_NAME,ENV_ACTION_SET,1);
        }
      }
      else {
          setenv(ENV_ACTION_NAME,ENV_ACTION_ACTIVE,1);
      }

      if (g_dump_actions == FALSE) {
        //need to add g_print_error_location_all, and processed location
        if (g_old_print_output == TRUE) {
          status
            = execute_list(c->_def.actions[result->_action].vtw_list_head,
                           &c->_def, NULL);
        }
        else {
          char *p = process_script_path(d->_path);
          status
            = execute_list(c->_def.actions[result->_action].vtw_list_head,
                           &c->_def, p);
          free(p);
        }
      } else {
        char buf[MAX_LENGTH_DIR_PATH*sizeof(char)];
        sprintf(buf,"%s\t:\t%s",ActionNames[result->_action],d->_path);
        if (c->_def.multi) {
          /* need to handle the embedded multinode as a special
           * case--should be fixed!
           */
          char *val = (char*)clind_unescape(d->_name);
          strcat(buf,val);
          free(val);
        }
        fprintf(out_stream,"%s\n",buf);
        status = 1;
      }
      if (result->_action == delete_act) {
        set_in_delete_action(FALSE);
      }

      unsetenv(ENV_ACTION_NAME);
      unsetenv(ENV_SIBLING_POSITION);
      unsetenv(ENV_DATA_PATH);

      if (g_coverage) {
        struct timeval t;
        gettimeofday(&t,NULL);
        fprintf(out_stream,"[END] %lu:%lu\n",t.tv_sec,t.tv_usec);
      }

      if (!status) { //EXECUTE_LIST RETURNS FALSE ON FAILURE....
        syslog(LOG_ERR, "commit error for %s:[%s]",
               ActionNames[result->_action],d->_path);
        if (g_display_error_node) {
          fprintf(out_stream, "%s@_errloc_:[%s]\n",
                  ActionNames[result->_action], d->_path);
        }
        result->_err_code = 1;
        d_dplog("commit2::process_func(): FAILURE: status: %d", status);
        return TRUE; //WILL STOP AT THIS POINT
      }
    }
  }
  return FALSE;
}

/**
 *
 **/
char*
process_script_path(char* in)
{
  if (in == NULL) {
    return NULL;
  }
  
  //just need to convert slashes into spaces here
  char path_buf[4096];
  char tmp[4096];
  char *ptr;
  path_buf[0] = '\0';
  
  strcpy(tmp,in);
  ptr = (char*)tmp;
  char *slash = strchr(tmp,'/');
  if (slash == NULL) {
    strcat(path_buf,in);
  }
  else {
    do {       //convert '/' to ' '
      strncat(path_buf,ptr,slash - ptr);
      strcat(path_buf," ");
      ++slash;
      ptr = slash;
    } while ((slash = strchr(slash,'/')) != NULL);
  }
  char *p = clind_unescape(path_buf);

  char tmp2[MAX_LENGTH_DIR_PATH*sizeof(char)];

  strcpy(tmp2,p); //copy back
  free(p);
  if (strncmp(ptr,"value:",6) == 0) {
    if (strlen(ptr)-6 > 0) {
      strncat(tmp2,ptr+6,strlen(ptr)-6);
      strcat(tmp2," ");
    }
  }
  char *ret = (char *) malloc(strlen(tmp2)+1);
  strcpy(ret,tmp2);
  return ret;
}

/**
 *
 **/
boolean
complete(GSList *node_coll, boolean test_mode)
{
  GSList *l;
  for (l = node_coll; l; l = g_slist_next (l)) {
    struct VyattaNode *gp = (struct VyattaNode *) ((GNode *) l->data)->data;
    const char *np = gp->_data._name;
    d_dplog("commit2::complete():name: [%s], path: [%s]",
            (np ? np : ""), (np ? gp->_data._path : ""));
    /* on transactional nodes only, note to avoid calling this if a
     * headless root
     */
    common_commit_copy_to_live_config((GNode *) l->data, FALSE, test_mode);
  }
  return TRUE;
}

/**
 *
 **/
gboolean
sort_func_priority(GNode *node, gpointer data)
{
  return sort_func(node,data,TRUE);
}

/**
 *
 **/
gboolean
sort_func_simple(GNode *node, gpointer data)
{
  return sort_func(node,data,FALSE);
}

/**
 *
 **/
gboolean
sort_func_priority_extended(GNode *node, gpointer data)
{
  struct VyattaNode *gp = (struct VyattaNode *) node->data;
  struct Config *gcfg = &(gp->_config);
  GNode *root_node = (GNode*)data;

  //WILL STOP AT DEPTH OF 10 REFERENCES
  //GET PARENT WORKING FIRST....

  //change action state of node according to enclosing behavior
  if (gcfg->_priority_extended) {
    //only if priority is specified.
    GNode *new_node = g_node_copy(node);

    //NOW, we need to figure out where this node belongs in the priority chain
    if (strncmp(gcfg->_priority_extended, "PARENT", 6) == 0) {
      //needs to walk up parents until priority is found and insert there....
      GNode *n = node;
      while (TRUE) {
        n = n->parent;
        if (n == NULL) {
          break;
        }
        gpointer nd = ((GNode*)n)->data;
        if (((struct VyattaNode*)nd)->_config._priority != LOWEST_PRIORITY) {
          //means we are done--found anchor in parent
          g_node_unlink(node);
          if (IS_DELETE(gp->_data._operation)) {
            g_node_insert_before(root_node,n,new_node);
          }
          else {
            g_node_insert_after(root_node,n,new_node);
          }
          break;
        }
      }
    }
  }
  return FALSE;
}

/**
 *
 **/
gboolean
sort_func(GNode *node, gpointer data, boolean priority_mode)
{
  struct VyattaNode *gp = (struct VyattaNode *) node->data;
  struct Data *d = &(gp->_data);
  GNode *root_node = (GNode*)data;
  d_dplog("commit2::sort_func(): %s, node count: %d",
          (d->_name ?  d->_name : "[n/a]"), g_node_n_children(root_node));

  //change action state of node according to enclosing behavior
  /* XXX this is ugly. originally the condition for the if is the following:
   *       (c1 && c2 || (c3 || c4) && c5)
   *
   *     this causes compiler warning for mixing && and || without (). the
   *     previous warning removal attempt changed the condition to the
   *     following:
   *       ((c1 && c2) || (c3 || (c4 && c5)))
   *
   *     which was incorrect (c3 and c4 should be at the same "level") and
   *     therefore was reverted.
   *
   *     now changing the condition to the following to avoid compiler
   *     warning:
   *       ((c1 && c2) || ((c3 || c4) && c5))
   *
   *     note that since the current goal is simply cleanup, no attempt is
   *     made to understand the logic here, and the change is purely based
   *     on operator precendence to maintain the original logic.
   *
   * XXX now removing deactivate-handling code, which involves c2.
   *
   *     note that c2 is (d->_disable_op != K_NO_DISABLE_OP), which means
   *     the node is "deactivated" (in working or active config or both).
   *     this in turn means that the (c1 && c2) part of the logic can only
   *     be true if the node is deactivated.
   *
   *     however, since activate/deactivate has not actually been exposed,
   *     this means that in actual usage the (c1 && c2) part is never true.
   *     therefore, we can simply remove the whole part, and the logic
   *     becomes:
   *       ((c3 || c4) && c5)
   */
  NODE_OPERATION op = d->_operation;
  if (((/* c3 */ IS_SET_OR_CREATE(op)) || (/* c4 */ IS_DELETE(op)))
      && (/* c5 */ IS_NOOP(((struct VyattaNode*)
                             (node->parent->data))->_data._operation))) {
    //first check if there is enclosing behavior
    boolean enclosing = FALSE;
    GNode *n = node;
    while (TRUE) {
      n = n->parent;
      vtw_def def = ((struct VyattaNode*)(n->data))->_config._def;
      if (def.actions[end_act].vtw_list_head
          || def.actions[begin_act].vtw_list_head) {
        enclosing = TRUE;
        break;
      }
      if (G_NODE_IS_ROOT(n) == TRUE) {
        break;
      }
    }

    //walk back up and flip operations until enclosing behavior
    if (enclosing == TRUE) {
      GNode *n = node;
      while (TRUE) {
        n = n->parent;
        vtw_def def = ((struct VyattaNode*)(n->data))->_config._def;
        if (((struct VyattaNode*)(n->data))->_data._operation == K_NO_OP) {
          /* XXX this is ugly. _operation is intended to be a bitmap, in which
           *     case it doesn't make sense to make it an enum type (should
           *     just be, e.g., int). this causes g++ to (rightly) complain.
           *     work around it for now to avoid impacting other code since
           *     the current goal is simply cleanup.
           */
          int op = ((struct VyattaNode*)(n->data))->_data._operation;
          op |= K_ACTIVE_OP;
          ((struct VyattaNode*)(n->data))->_data._operation
            = (NODE_OPERATION) op;
        }
        if (def.actions[end_act].vtw_list_head
            || def.actions[begin_act].vtw_list_head) {
          break;
        }
        if (G_NODE_IS_ROOT(n) == TRUE) {
          break;
        }
      }
    }
  }

  if (priority_mode) {
    int gprio = gp->_config._priority;
    if (gprio < LOWEST_PRIORITY) {
      // only if priority is specified.
      //unlink from original tree
      g_node_unlink(node);

      GNode *new_node = g_node_copy(node);
      GNode *sibling = root_node->children;
      //now iterate through siblings of root_node and compare priority
      
      while (sibling
             && gprio > ((struct VyattaNode*)(sibling->data))
                        ->_config._priority) {
        sibling = sibling->next;
        if (!sibling
            || gprio < ((struct VyattaNode*)(sibling->data))
                       ->_config._priority) {
          // XXX isn't this redundant??? just cleaning up so not changing it
          break;
        }
      }

      d_dplog("commit2::sort_func(): inserting %s into transaction, "
              "priority: %d BEFORE %d", d->_name, gprio,
              (sibling
               ? ((struct VyattaNode*)(sibling->data))->_config._priority
                 : LOWEST_PRIORITY));
      g_node_insert_before(root_node,sibling,new_node);
    }
  }
  else {
    if (g_node_depth(node) == 2) {
      d_dplog("commit2::sort_func(): insert %s into transaction", d->_name);
      GNode *new_node = g_node_copy(node);
      g_node_insert(root_node,-1,new_node); //make a flat structure for now
    }
  }
  return FALSE;
}

/**
 * Gets a flat collection of nodes, sorted by priority
 **/
GNode*
get_transactions(GNode *config, boolean priority_mode)
{
  d_dplog("commit2::get_transactions()");
  if (config == NULL) {
    return NULL;
  }

  gpointer gp = ((GNode*)config)->data;

  GNode *trans_root = g_node_new(gp);
  if (priority_mode) {
    g_node_traverse(config,
                    G_POST_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)sort_func_priority,
                    (gpointer)trans_root);

    //only do this if the root isn't empty
    if (g_node_n_children(config) != 0) {
      g_node_insert(trans_root,-1,config); //add what's left
    }

    //now need pass to handle extended priority system
    g_node_traverse(trans_root,
                    G_POST_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)sort_func_priority_extended,
                    (gpointer)trans_root);
  }
  else {
    g_node_traverse(config,
                    G_IN_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)sort_func_simple,
                    (gpointer)trans_root);
  }
  return trans_root;
}

/**
 *
 **/
static gboolean
cleanup_func(GNode *node, gpointer data)
{
  struct VyattaNode *vn = ((struct VyattaNode*)(node->data));
  if (vn->_data._name) {
    free(vn->_data._name);
  }
  if (vn->_data._path) {
    free(vn->_data._path);
  }
  if (vn->_config._help) {
    free(vn->_config._help);
  }
  if (vn->_config._default) {
    free(vn->_config._default);
  }
  if (vn->_config._path) {
    free(vn->_config._path);
  }
  return FALSE;
}

/**
 *
 **/
void
cleanup(GNode *root_node) 
{
  if (root_node == NULL) {
    return;
  }

  g_node_traverse(root_node,
                  G_IN_ORDER,
                  G_TRAVERSE_ALL,
                  -1,
                  (GNodeTraverseFunc)cleanup_func,
                  (gpointer)NULL);

  g_node_destroy(root_node);
}

/**
 *
 **/
gboolean
dump_func(GNode *node, gpointer data)
{
  FILE *out;
  if (g_dump_trans) {
    out = out_stream;
  }
  else {
    out = stdout;
  }

  if (node != NULL) {
    guint depth = g_node_depth(node);
    if (depth == 2) {
      fprintf(out,"NEW TRANS\n");
    }

    struct VyattaNode *gp = (struct VyattaNode *) node->data;
    struct Data *gdata = &(gp->_data);
    struct Config *gcfg = &(gp->_config);
    if (gdata->_name != NULL) {
      unsigned int i;
      NODE_OPERATION op = gdata->_operation;
      if (IS_ACTIVE(op)) {
        fprintf(out, "*");
      } else if (IS_DELETE(op)) {
        fprintf(out, "-");
      } else if (IS_CREATE(op)) {
        fprintf(out, "+");
      } else if (IS_SET(op)) {
        fprintf(out, ">");
      } else {
        fprintf(out, " ");
      }

      for (i = 0; i < depth; ++i) {
        fprintf(out,"  ");
      }

      if (gcfg->_def.def_type2 != ERROR_TYPE) {
        fprintf(out,"%s (t: %d-%d, ", gdata->_name, gcfg->_def.def_type,
                gcfg->_def.def_type2);
      } else {
        fprintf(out,"%s (t: %d, ", gdata->_name, gcfg->_def.def_type);
      }
      if (gcfg->_priority_extended) {
        fprintf(out, "p: %s)", gcfg->_priority_extended);
      } else {
        fprintf(out, "p: %d)", gcfg->_priority);
      }

      if (gdata->_value == TRUE) {
        fprintf(out," [VALUE]");
      }
      if (gcfg->_multi == TRUE) {
        fprintf(out," [MULTI(%d)]",gcfg->_limit);
      }
      if (gcfg->_def.actions[syntax_act].vtw_list_head
          && !gcfg->_def.actions[syntax_act].vtw_list_head->vtw_node_aux) {
        fprintf(out," [SYNTAX]");
      }
      if (gcfg->_def.actions[create_act].vtw_list_head) {
        fprintf(out," [CREATE]");
      }
      if (gcfg->_def.actions[activate_act].vtw_list_head) {
        fprintf(out," [ACTIVATE]");
      }
      if (gcfg->_def.actions[update_act].vtw_list_head) {
        fprintf(out," [UPDATE]");
      }
      if (gcfg->_def.actions[delete_act].vtw_list_head) {
        fprintf(out," [DELETE]");
      }
      if (gcfg->_def.actions[syntax_act].vtw_list_head
          && gcfg->_def.actions[syntax_act].vtw_list_head->vtw_node_aux) {
        fprintf(out," [COMMIT]");
      }
      if (gcfg->_def.actions[begin_act].vtw_list_head) {
        fprintf(out," [BEGIN]");
      }
      if (gcfg->_def.actions[end_act].vtw_list_head) {
        fprintf(out," [END]");
      }
      fprintf(out,"\n");
    }
  }
  return FALSE;
}

/**
 *
 **/
boolean
process_priority_node(GNode *priority_node)
{
  /* on each node that is deleted run the delete action within the context
   * of the transaction
   */
  struct Result result;
  result._err_code = 0;

  if (priority_node == NULL) {
    return FALSE;
  }

  //if this node is an enclosing node, we'll skip this iteration
  struct VyattaNode *gp = (struct VyattaNode *) priority_node->data;
  struct Config *c = &(gp->_config);
  //does this node contain a begin or end statement?
  boolean priority_node_is_enclosing_node = FALSE;
  if (c->_def.actions  && (c->_def.actions[end_act].vtw_list_head
                           || c->_def.actions[begin_act].vtw_list_head)) {
    priority_node_is_enclosing_node = TRUE;
  }

  if (priority_node_is_enclosing_node == FALSE) {
    //traverse priority node from children up
    g_node_traverse((GNode*)priority_node,
                    G_POST_ORDER,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)enclosing_process_func,
                    (gpointer)&result);
    
    if (result._err_code != 0) {
      return FALSE;
    }
  }
  /* now perform processing on what's left outside of the enclosing
   * begin/end statements
   */
  int i;
  for (i = 0; i < g_num_actions; ++i) {
    // now _this_ should be enum instead
    GTraverseType order;
    if (delete_act != ActionOrder[i]) {
      order = G_PRE_ORDER;
    }
    else {
      order = G_POST_ORDER;
    }
    
    result._action = ActionOrder[i];
    g_node_traverse((GNode*)priority_node,
                    order,
                    G_TRAVERSE_ALL,
                    -1,
                    (GNodeTraverseFunc)process_func,
                    (gpointer)&result);

    if (result._err_code != 0) {
      d_dplog("commit2::process_priority_node(): failure on "
              "processing pass: %d", i);
      return FALSE;
    }
  }
  return TRUE;
}

/**
 * Look for begin/end statements to begin processing
 * of actions.
 **/
static gboolean
enclosing_process_func(GNode *node, gpointer data)
{
  if (node == NULL) {
    return TRUE;
  }

  struct Result *result = (struct Result*)data;
  struct VyattaNode *gp = (struct VyattaNode *) node->data;
  struct Config *c = &(gp->_config);
  struct Data *d = &(gp->_data);

  //does this node contain a begin or end statement?
  if (c->_def.actions  && (c->_def.actions[end_act].vtw_list_head
                           || c->_def.actions[begin_act].vtw_list_head)) {
    /* gotten to this point need to do a call around this enclosing
     * being/end node
     */
    g_node_unlink(node); //removed this...

    d_dplog("commit2::enclosing_process_func(): enclosing statement found "
            "on: %s", d->_path);
    //perform recursive calling on new process node...

    int i;
    for (i = 0; i < g_num_actions; ++i) {
      // again should be enum
      GTraverseType order;
      if (delete_act != ActionOrder[i]) {
        order = G_PRE_ORDER;
      }
      else {
        order = G_POST_ORDER;
      }

      result->_action = ActionOrder[i];
      g_node_traverse((GNode*)node,
                      order,
                      G_TRAVERSE_ALL,
                      -1,
                      (GNodeTraverseFunc)process_func,
                      (gpointer)result);
    
      if (result->_err_code != 0) {
        //EXECUTE_LIST RETURNS FALSE ON FAILURE....
        d_dplog("commit2::enclosing_process_func(): FAILURE: status: %d",
                result->_err_code);
        return TRUE; //WILL STOP AT THIS POINT
      }
    }
  }
  return FALSE;
}

/**
 *
 **/
static boolean
validate_configuration(GNode *root_node, boolean mode,
                       GSList **nodes_visited_coll) 
{
  if (root_node == NULL) {
    return FALSE;
  }

  struct Result result;
  result._err_code = 0;
  result._mode = (int)mode;
  result._data = (void*)*nodes_visited_coll;

  //handles both syntax and commit
  result._action = syntax_act;
  g_node_traverse((GNode*)root_node,
                  G_PRE_ORDER,
                  G_TRAVERSE_ALL,
                  -1,
                  (GNodeTraverseFunc)validate_func,
                  (gpointer)&result);
  
  if (result._err_code != 0) {
    d_dplog("commit2::process_priority_node(): failure on processing "
            "pass: %d", syntax_act);
    return FALSE;
  }

  GList **c_tmp = (GList**)result._data;
  *nodes_visited_coll = (GSList*)c_tmp;
  return TRUE;
}

/**
 * Execute syntax and commit checks
 **/
static gboolean
validate_func(GNode *node, gpointer data)
{
  if (node == NULL) {
    return TRUE;
  }

  struct VyattaNode *gp = (struct VyattaNode *) node->data;
  struct Config *c = &(gp->_config);
  struct Data *d = &(gp->_data);
  struct Aux *a = &(gp->_aux);
  struct Result *result = (struct Result*)data;

  /* let's mark first last nodes here for use later
   * do first/last/only sibling check, restrict to nodes with operations
   * defined
   */
  GNode *n_last_op = NULL;
  GNode *n_first_op = NULL;
  
  GNode *sib = g_node_first_sibling(node);
  while (sib != NULL) {
    if (IS_DELETE(((struct VyattaNode*)(sib->data))->_data._operation)) {
      if (n_first_op == NULL) {
        n_first_op = sib;
      }
      n_last_op = sib;
    }
    sib = sib->next;
  }

  sib = g_node_first_sibling(node);
  while (sib != NULL) {
    if (IS_SET_OR_CREATE(((struct VyattaNode*)(sib->data))
                         ->_data._operation)) {
      if (n_first_op == NULL) {
        n_first_op = sib;
      }
      n_last_op = sib;
    }
    sib = sib->next;
  }

  a->_first = (node == n_first_op);
  a->_last = (node == n_last_op);


  /* since this visits all working nodes, let's maintain a set of nodes
   * to commit
   */
  GSList *coll = (GSList*)result->_data;
  if (d->_path != NULL) {
    char buf[MAX_LENGTH_DIR_PATH*sizeof(char)];
    if (IS_DELETE(d->_operation)) {
      sprintf(buf,"- %s",d->_path);
      if (c->_def.multi) {
        /* need to handle the embedded multinode as a special
         * case--should be fixed!
         */
        char *val = (char*)clind_unescape(d->_name);
        strcat(buf,val);
        free(val);
      }
      char *tmp = (char *) malloc(strlen(buf)+1);
      strcpy(tmp,buf);
      coll = g_slist_append(coll,tmp);
      result->_data = (void*)coll;
    }
    else if (IS_SET_OR_CREATE(d->_operation)) {
      sprintf(buf,"+ %s",d->_path);
      if (c->_def.multi) {
        /* need to handle the embedded multinode as a special
         * case--should be fixed!
         */
        char *val = (char*)clind_unescape(d->_name);
        strcat(buf,val);
        free(val);
      }
 
      char *tmp = (char *) malloc(strlen(buf)+1);
      strcpy(tmp,buf);
      coll = g_slist_append(coll,tmp);
      result->_data = (void*)coll;
    }
  }
  
  //don't run syntax check on this node if it is unchanged.
  if (IS_NOOP(d->_operation) && c->_def.actions[syntax_act].vtw_list_head
      && !c->_def.actions[syntax_act].vtw_list_head->vtw_node_aux) {
    return FALSE;
  }
  
  if (IS_DELETE(d->_operation) && !IS_ACTIVE(d->_operation)) {
    return FALSE; //will not perform validation checks on deleted nodes
  }

  if (!c->_def.actions  ||
      !c->_def.actions[result->_action].vtw_list_head){
    return FALSE;
  }

  /* will not call term multi if it is a noop--shouldn't show up in tree
   * in the first place, but will require more rework of unionfs code
   * to fix this.
   */
  if (c->_def.multi && IS_NOOP(d->_operation)) {
    return FALSE;
  }
  
  //look at parent for multi tag
  if (d->_value && d->_name) {
    char *val = d->_name;
    if (c->_def.tag) {
      /* need to handle the embedded multinode as a special
       * case--should be fixed!
       */
      val = (char*)clind_unescape(d->_name);
    }
    d_dplog("commit2::process_func(): @ value: %s",(char *) val);

    set_at_string(val); //embedded multinode value
  }
  else {
    if (g_debug) {
      dplog("commit2::process_func(): boolean value is: %d", d->_value);
      if (node->parent
          && ((struct VyattaNode*)(node->parent->data))->_data._name) {
        dplog("commit2::process_func(): parent has a name: %s",
              ((struct VyattaNode*)(node->parent->data))->_data._name);
      }
      dplog("commit2::process_func(): @ value: [NULL]");
    }
  }

  common_set_context(c->_path,d->_path);
  d_dplog("Executing %s on this node", ActionNames[result->_action]);
  
  if (g_coverage) {
    struct timeval t;
    gettimeofday(&t,NULL);
    fprintf(out_stream, "[START] %lu:%lu, %s@%s", (unsigned long) t.tv_sec,
            (unsigned long) t.tv_usec, ActionNames[result->_action], d->_path);
  }

  boolean status = 1;
  if (g_dump_actions == FALSE) {
    //set location env
    setenv(ENV_DATA_PATH,d->_path,1);
    if (g_old_print_output == TRUE) {
      status
        = execute_list(c->_def.actions[result->_action].vtw_list_head,
                       &c->_def, NULL);
    } else {
      char *p = process_script_path(d->_path);
      status
        = execute_list(c->_def.actions[result->_action].vtw_list_head,
                       &c->_def, p);
      free(p);
    }
    unsetenv(ENV_DATA_PATH);
  }
  else {
    char buf[MAX_LENGTH_DIR_PATH*sizeof(char)];
    if (c->_def.actions[syntax_act].vtw_list_head) {
      if (c->_def.actions[syntax_act].vtw_list_head->vtw_node_aux == 0) {
        sprintf(buf,"syntax\t:\t%s",d->_path);
      }
      else {
        sprintf(buf,"commit\t:\t%s",d->_path);
      }
    }
    if (c->_def.multi) {
      /* need to handle the embedded multinode as a special
       * case--should be fixed!
       */
      char *val = (char*)clind_unescape(d->_name);
      strcat(buf,val);
      free(val);
    }
    fprintf(out_stream,"%s\n",buf);
    status = 1;
  }

  if (g_coverage) {
    struct timeval t;
    gettimeofday(&t,NULL);
    fprintf(out_stream,"[END] %lu:%lu\n",t.tv_sec,t.tv_usec);
  }
  
  if (!status) { //EXECUTE_LIST RETURNS FALSE ON FAILURE....
    syslog(LOG_ERR, "commit error for %s:[%s]",
           ActionNames[result->_action], d->_path);
    if (g_display_error_node) {
      fprintf(out_stream, "%s@_errloc_:[%s]\n",
              ActionNames[result->_action], d->_path);
    }
    result->_err_code = 1;
    d_dplog("commit2::validate_func(): FAILURE: status: %d", status);
    // WILL STOP AT THIS POINT if mode is not set for full syntax check
    return result->_mode ? FALSE: TRUE;
  }
  return FALSE;
}

/**
 *
 **/
static void
update_change_file(FILE *fp, GSList *coll) 
{
  if (coll == NULL || fp == NULL) {
    return;
  }
  GSList *l;
  for (l = coll; l; l = g_slist_next (l)) {
    if (l->data) {
      char buf[MAX_LENGTH_DIR_PATH*sizeof(char)];
      sprintf(buf,"%s\n",(char*)l->data);
      fwrite(buf,1,strlen((char*)buf),fp);
      free(l->data);
    }
  }
  g_slist_free(coll);
}
