#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib-object.h>  /* g_type_init */
#include "common/common.h"

boolean g_debug = FALSE;

/**
 *
 *
 **/
void 
usage()
{
  printf("exe_action\n");
  printf("-p\t\trelative path to node.def\n");
  printf("-a\t\taction (integer value)\n");
  printf("-d\t\tdebug mode\n");
  printf("-h\t\thelp\n");
}

/**
 *
 *
 **/
int
main(int argc, char** argv)
{
  int ch;
  char *path = NULL;
  unsigned long act = 0;

  /* this is needed before calling certain glib functions */
  #if !GLIB_CHECK_VERSION(2,35,0)
  g_type_init();
  #endif

  //grab inputs
  while ((ch = getopt(argc, argv, "dhp:a:")) != -1) {
    switch (ch) {
    case 'd':
      g_debug = TRUE;
      break;
    case 'h':
      usage();
      exit(0);
      break;
    case 'p':
      path = optarg;
      break;
    case 'a':
      act = strtoul(optarg,NULL,10);
      break;
    default:
      usage();
      exit(0);
    }
  }


  vtw_def def;
  struct stat s;
  const char *root = get_tmpp();
  char buf[2048];
  sprintf(buf,"%s/%snode.def",root,path);
  printf("%s\n",buf);
  initialize_output(NULL);
  init_paths(TRUE);

  printf("[path: %s][act: %lu]\n",buf,act);

  if ((lstat(buf,&s) == 0) && S_ISREG(s.st_mode)) {
    if (parse_def(&def,buf,FALSE) == 0) {
      //execute
      int status;
      if (def.actions[act].vtw_list_head) {
	set_in_commit(TRUE);

	char foo[1024];
	sprintf(foo,"b");
	set_at_string(foo);

	//BROKEN--NEEDS TO BE FIX BELOW FOR DPATH AND CPATH
	common_set_context(path,path);

	status = execute_list(def.actions[act].vtw_list_head, &def, NULL);
	if (status == FALSE) {
	  printf("command failed! status: %d\n", status);
	}
	else {
	  printf("SUCCESS!\n");
	}
	set_in_commit(FALSE);
      }
      else {
	printf("no action for this node\n");
      }
    }
    else {
      printf("failure to parse defination file\n");
    }
  }
  else {
    printf("node.def not found at: %s\n",buf);
  }
  return 0;
}
