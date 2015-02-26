#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "srs2.h"
#include "spf2/spf.h"
#include "libmilter/mfapi.h"


#define SRS_MILTER_NAME "srs-milter"
#define SRS_MILTER_VERSION "0.0.2"

#define SS_STATE_NULL             0x00
#define SS_STATE_INVALID_CONN     0x01
#define SS_STATE_INVALID_MSG      0x02

#define MAXBUF 1024

/* Global variables */
static pthread_key_t key;
static int connections = 0;
static int threads = 0;

/* these should be read from command line or config file */
typedef struct {
  int verbose;
  char *pidfile;
  int forward;
  int reverse;
  char *socket;
  int timeout;
  char **domains;
  int spf_check;
  char *spf_heloname;
  union {
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
  } spf_address;
  int spf_address_defined;
  char *srs_domain;
  char **srs_secrets;
  int srs_alwaysrewrite;
  int srs_hashlength;
  int srs_hashmin;
  int srs_maxage;
  char srs_separator;
} config_t;

config_t config;


/* Per-connection data structure. */
struct srs_milter_connection_data {
  int num;
  int state;
  char* sender;
  char** envfromargv;
  char** recip;
  int recip_remote;
};
/* Per-thread data structure. */
struct srs_milter_thread_data {
  int num;
  srs_t *srs;
  SPF_server_t *spf;
};



int srs_milter_configure(const char *key, const char *val) {
#define CHECK_VALUE(key) \
    if (val == NULL || *val == '\0') { \
      fprintf(stderr, "ERROR: missing %s value\n", key); \
      return 1; \
    }

  if (strcmp(key, "verbose") == 0) {
    config.verbose = 1;

  } else if (strcmp(key, "pidfile") == 0) {
    CHECK_VALUE(key);

    if (config.pidfile)
      free(config.pidfile);
    config.pidfile = strdup(val);

  } else if (strcmp(key, "socket") == 0) {
    CHECK_VALUE(key);

    if (config.socket)
      free(config.socket);
    config.socket = strdup(val);

  } else if (strcmp(key, "timeout") == 0) {
    CHECK_VALUE(key);

    config.timeout = atoi(val);

  } else if (strcmp(key, "forward") == 0) {
    config.forward = 1;

  } else if (strcmp(key, "reverse") == 0) {
    config.reverse = 1;

  } else if (strcmp(key, "local-domain") == 0) {
    CHECK_VALUE(key);

    int i = 0;
    if (!config.domains) {
      config.domains = (char **) malloc((i+2)*sizeof(char *));
    } else {
      while (config.domains[i]) i++;
      config.domains = (char **) realloc(config.domains, (i+2)*sizeof(char *));
    }
    config.domains[i] = strdup(val);
    config.domains[i+1] = NULL;

  } else if (strcmp(key, "srs-domain") == 0) {
    CHECK_VALUE(key);

    if (config.srs_domain)
      free(config.srs_domain);
    config.srs_domain = strdup(val);

  } else if (strcmp(key, "srs-secret") == 0) {
    CHECK_VALUE(key);

    int i = 0;
    if (!config.srs_secrets) {
      config.srs_secrets = (char **) malloc((i+2)*sizeof(char *));
    } else {
      while (config.srs_secrets[i]) i++;
      config.srs_secrets = (char **) realloc(config.srs_secrets, (i+2)*sizeof(char *));
    }
    config.srs_secrets[i] = strdup(val);
    config.srs_secrets[i+1] = NULL;

  } else if (strcmp(key, "srs-alwaysrewrite") == 0) {
    config.srs_alwaysrewrite = 1;

  } else if (strcmp(key, "srs-hashlength") == 0) {
    CHECK_VALUE(key);

    config.srs_hashlength = atoi(val);

  } else if (strcmp(key, "srs-hashmin") == 0) {
    CHECK_VALUE(key);

    config.srs_hashmin = atoi(val);

  } else if (strcmp(key, "srs-maxage") == 0) {
    CHECK_VALUE(key);

    config.srs_maxage = atoi(val);

  } else if (strcmp(key, "srs-separator") == 0) {
    CHECK_VALUE(key);

    config.srs_separator = val[0];

  } else if (strcmp(key, "spf-check") == 0) {
    config.spf_check = 1;

  } else if (strcmp(key, "spf-heloname") == 0) {
    CHECK_VALUE(key);

    if (config.spf_heloname)
      free(config.spf_heloname);
    config.spf_heloname = strdup(val);

  } else if (strcmp(key, "spf-address") == 0) {
    CHECK_VALUE(key);

    if (inet_pton(AF_INET, val, &config.spf_address) <= 0)
      if (inet_pton(AF_INET6, val, &config.spf_address) <= 0) {
        fprintf(stderr, "ERROR: invalid SPF address %s\n", val);
        return 1;
      }

    config.spf_address_defined = 1;

  } else {
    fprintf(stderr, "WARN: unknown argument %s='%s'\n", key, val);

  }

  return 0;
}

int srs_milter_load_config(const char *filename) {
  char line[MAXBUF];
  int ret = 0;

  FILE *file = fopen (filename, "r");
  if (file == NULL) {
    fprintf(stderr, "ERROR: unable to open config file %s\n", filename);
    return 1;
  }

  /* parse config line */
  while(fgets(line, sizeof(line), file) != NULL) {
    char *key = line;
    char *val = NULL;
    char *pos = line+strlen(line)-1;

    if (strlen(line) > 0 && line[0] == '#')
      continue;

    while (pos >= key && (*pos == '\n' || *pos == ' ')) {
      *pos = '\0';
      pos--;
    }

    if (strlen(key) == 0)
      continue;

    pos = strchr(line, '=');
    if (pos) {
      val = pos+1;
      while (*val == ' ' && *val != '\0') val++;
      do {
        *pos = '\0';
        pos--;
      } while (pos >= key && *pos == ' ');
    }

    ret += srs_milter_configure(key, val);
  }

  fclose(file);

  return ret;
}

int is_local_addr(const char *addr) {
  int i, r;
  const char *dom;

  if (!addr)
    return 0;

  if (!config.domains)
    return 0;

  dom  = strrchr(addr, '@')+1;
  if (!dom)
    dom = addr;

  for (i = 0; config.domains[i]; i++) {

    // exact domain name match
    if (strcasecmp(dom, config.domains[i]) == 0)
      return 1;

    // match subdomain
    r = strlen(dom) - strlen(config.domains[i]);
    if (r > 0 && config.domains[i][0] == '.' && strcasecmp(dom + r, config.domains[i]) == 0)
      return 1;

  }

  return 0;
}



static void srs_milter_thread_data_destructor(void* data) {
  if (!data)
    return;

  struct srs_milter_thread_data* td = (struct srs_milter_thread_data*) data;

  if (td->srs) {
    srs_free(td->srs);
    td->srs = NULL;
  }

  if (td->spf) {
    SPF_server_free(td->spf);
    td->spf = NULL;
  }

  free(data);
}



// https://www.milter.org/developers/api/xxfi_connect
static sfsistat
xxfi_srs_milter_connect(SMFICTX* ctx, char *hostname, _SOCK_ADDR *hostaddr) {
  struct srs_milter_thread_data* td;
  struct srs_milter_connection_data* cd;

  // get/allocate thread specific data
  td = (struct srs_milter_thread_data*) pthread_getspecific(key);
  if (!td) {
    td = (struct srs_milter_thread_data*) malloc( sizeof(struct srs_milter_thread_data) );
    if (!td) {
      syslog(LOG_ERR, "conn# ?[?] - xxfi_srs_milter_connect(\"%s\", %p): can't allocate memory for thread data",
             hostname, hostaddr);
      return SMFIS_TEMPFAIL;
    }

    memset(td, '\0', sizeof(struct srs_milter_thread_data));
    td->num = ++threads; // this should be done in thread-safe way

    if (config.verbose)
      syslog(LOG_DEBUG, "conn# ?[?] - xxfi_srs_milter_connect(\"%s\", %p): created new thread %i data",
             hostname, hostaddr, td->num);

    if (pthread_setspecific(key, td)) {
      syslog(LOG_ERR, "conn# ?[?] - xxfi_srs_milter_connect(\"%s\", %p): can't store thread %i data",
             hostname, hostaddr, td->num);
      free(td);
      return SMFIS_TEMPFAIL;
    }
  }

  // allocate connection specific data
  cd = (struct srs_milter_connection_data*) malloc(sizeof(struct srs_milter_connection_data));
  if (!cd) {
    if (config.verbose)
      syslog(LOG_DEBUG, "conn# ?[?] - xxfi_srs_milter_connect(\"%s\", %p): can't allocate memory for connection data",
             hostname, hostaddr);
    return SMFIS_TEMPFAIL;
  }
  if (smfi_setpriv(ctx, (void*) cd) != MI_SUCCESS) {
    if (config.verbose)
      syslog(LOG_DEBUG, "conn# ?[?] - xxfi_srs_milter_connect(\"%s\", %p): can't set ctx data",
             hostname, hostaddr);
    return SMFIS_TEMPFAIL;
  }

  memset(cd, '\0', sizeof(struct srs_milter_connection_data));
  cd->state = SS_STATE_NULL;
  cd->num = ++connections; // this should be done in thread-safe way

  if (config.verbose)
    syslog(LOG_DEBUG, "conn# %d[%i] - xxfi_srs_milter_connect(\"%s\", hostaddr)",
           cd->num, cd->state, hostname);

  return SMFIS_CONTINUE;
}



// https://www.milter.org/developers/api/xxfi_envfrom
static sfsistat
xxfi_srs_milter_envfrom(SMFICTX* ctx, char** argv) {
  struct srs_milter_connection_data* cd =
          (struct srs_milter_connection_data*) smfi_getpriv(ctx);

  if (cd->state & SS_STATE_INVALID_CONN)
    return SMFIS_CONTINUE;

  if (config.verbose)
    syslog(LOG_DEBUG, "conn# %d[%i] - xxfi_srs_milter_envfrom(\"%s\")",
           cd->num, cd->state, argv[0]);

  if (strlen(argv[0]) < 1 || strcmp(argv[0], "<>") == 0 || argv[0][0] != '<' || argv[0][strlen(argv[0])-1] != '>' || !strchr(argv[0], '@')) {
    cd->state |= SS_STATE_INVALID_MSG;
    if (config.verbose)
      syslog(LOG_DEBUG, "conn# %d[%i] - xxfi_srs_milter_envfrom(\"%s\"): skipping \"MAIL FROM: %s\"",
             cd->num, cd->state, argv[0], argv[0]);
    return SMFIS_CONTINUE;
  }

  // cleanup data structure for new message
  // (there can be more messages send throught one connection,
  // so this structure could be filled by previous message)
  cd->state = SS_STATE_NULL;

  if (cd->sender) {
    free(cd->sender);
    cd->sender = NULL;
  }

  if (cd->envfromargv) {
    int i;
    for (i = 0; cd->envfromargv[i]; i++)
      free(cd->envfromargv[i]);
    free(cd->envfromargv);
    cd->envfromargv = NULL;
  }

  if (cd->recip) {
    int i;
    for (i = 0; cd->recip[i]; i++)
      free(cd->recip[i]);
    free(cd->recip);
    cd->recip = NULL;
  }

  cd->recip_remote = 0;

  // strore MAIL FROM: address
  cd->sender = (char *) malloc(strlen(argv[0])-1);
  if (!cd->sender) {
    // memory allocation problem
    cd->state |= SS_STATE_INVALID_MSG;
    return SMFIS_CONTINUE;
  }
  strncpy(cd->sender, argv[0]+1, strlen(argv[0])-2);
  cd->sender[strlen(argv[0])-2] = '\0';

  // store MAIL FROM: arguments
  {
    int argc = 0;
    while (argv[argc]) argc++;
    cd->envfromargv = (char **) malloc((argc+1)*sizeof(char *));
    for (argc = 0; argv[argc]; argc++) {
      cd->envfromargv[argc] = strdup(argv[argc]);
      if (!cd->envfromargv[argc]) {
        // memory allocation problem
        cd->state |= SS_STATE_INVALID_MSG;
        return SMFIS_CONTINUE;
      }
    }
    cd->envfromargv[argc] = NULL;
  }

  return SMFIS_CONTINUE;
}



// https://www.milter.org/developers/api/xxfi_envrcpt
static sfsistat
xxfi_srs_milter_envrcpt(SMFICTX* ctx, char** argv) {
  struct srs_milter_connection_data* cd =
          (struct srs_milter_connection_data*) smfi_getpriv(ctx);

  if (cd->state & SS_STATE_INVALID_CONN)
    return SMFIS_CONTINUE;

  if (config.verbose)
    syslog(LOG_DEBUG, "conn# %d[%i] - xxfi_srs_milter_envrcpt(\"%s\")",
           cd->num, cd->state, argv[0]);

  // get recipient address
  char *recip = (char *) malloc(strlen(argv[0])-1);
  if (!recip) {
    // memory allocation problem
    cd->state |= SS_STATE_INVALID_MSG;
    return SMFIS_CONTINUE;
  }
  strncpy(recip, argv[0]+1, strlen(argv[0])-2);
  recip[strlen(argv[0])-2] = '\0';

  if (!is_local_addr(recip)) {
    cd->recip_remote = 1;
  } else {
    // list of local SRS recipient addresses that should be reversed
    if (recip[0] != '\0' && recip[1] != '\0' 
        && recip[2] != '\0' && recip[3] != '\0'
        && SRS_IS_SRS_ADDRESS(recip)) {
      int argc = 0;

      if (!cd->recip) {
        cd->recip = (char **) malloc((argc+2)*sizeof(char *));
      } else {
        while (argv[argc]) argc++;
        cd->recip = (char **) realloc(cd->recip, (argc+2)*sizeof(char *));
      }

      if (!cd->recip) {
        // memory allocation problem
        cd->state |= SS_STATE_INVALID_MSG;
      } else {
        cd->recip[argc] = strdup(argv[0]);
        cd->recip[argc+1] = NULL;
        if (!cd->recip[argc]) {
          // memory allocation problem
          cd->state |= SS_STATE_INVALID_MSG;
        }
      }
    }
  }

  free(recip);

  return SMFIS_CONTINUE;
}



// https://www.milter.org/developers/api/xxfi_eom
static sfsistat
xxfi_srs_milter_eom(SMFICTX* ctx) {
  struct srs_milter_connection_data* cd =
          (struct srs_milter_connection_data*) smfi_getpriv(ctx);

  if (cd->state & (SS_STATE_INVALID_CONN | SS_STATE_INVALID_MSG))
    return SMFIS_CONTINUE;

  char *queue_id = smfi_getsymval(ctx, "{i}");
  if (!queue_id) queue_id = "unknown";

  if (config.verbose)
    syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom()", cd->num, cd->state, queue_id);

  int fix_envfrom = 0;

  // non-local sender to non-local recipient
  // SPF can prevent forwarding, check if it is the case
  // for this particular sender domain
  if (config.forward && !is_local_addr(cd->sender) && cd->recip_remote) {

    if (config.srs_alwaysrewrite || !config.spf_check) {

      fix_envfrom = 1;

    } else {
      // check if non-local MAIL FROM: sender domain has SPF data in DNS
      struct srs_milter_thread_data* td;
      SPF_request_t *spf_request = NULL;
      SPF_response_t *spf_response = NULL;
      SPF_errcode_t spf_ret = SPF_E_SUCCESS;
      char host[INET_ADDRSTRLEN+1];

      td = (struct srs_milter_thread_data*) pthread_getspecific(key);
      if (!td) {
        syslog(LOG_ERR, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): can't get thread data!? (SPF)",
               cd->num, cd->state, queue_id);
        cd->state |= SS_STATE_INVALID_MSG;
        return SMFIS_CONTINUE;
      }

      while (1) {
        if (!td->spf) {
          if (config.verbose)
            syslog(LOG_DEBUG, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): SPF_server_new",
                   cd->num, cd->state, queue_id, td->num);

          if (!(td->spf = SPF_server_new(SPF_DNS_CACHE, config.verbose))) {
            syslog(LOG_NOTICE, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): libspf2 error SPF_server_new",
                   cd->num, cd->state, queue_id, td->num);
            break;
          }

          // char *site;
          // if ((site = smfi_getsymval(ctx, "j")))
          //   SPF_server_set_rec_dom(spf_server, site);
          // else
          //  SPF_server_set_rec_dom(spf_server, "localhost");
        }

        if (!(spf_request = SPF_request_new(td->spf))) {
          syslog(LOG_NOTICE, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): libspf2 error SPF_request_new",
                 cd->num, cd->state, queue_id, td->num);
          break;
        }

        if (config.spf_address.in.sin_family == AF_INET) {
          SPF_request_set_ipv4(spf_request, config.spf_address.in.sin_addr);
          inet_ntop(AF_INET, &config.spf_address.in.sin_addr, host, sizeof(host));
        } else {
          SPF_request_set_ipv6(spf_request, config.spf_address.in6.sin6_addr);
          inet_ntop(AF_INET6, &config.spf_address.in6.sin6_addr, host, sizeof(host));
        }

        spf_ret = SPF_request_set_helo_dom(spf_request, config.spf_heloname);
        if (spf_ret != SPF_E_SUCCESS) break;
        spf_ret = SPF_request_set_env_from(spf_request, cd->sender);
        if (spf_ret != SPF_E_SUCCESS) break;
        spf_ret = SPF_request_query_mailfrom(spf_request, &spf_response);
        if (spf_ret != SPF_E_SUCCESS) break;

        if (spf_response) {
          SPF_result_t spf_result = SPF_response_result(spf_response);
          if (config.verbose)
            syslog(LOG_DEBUG, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): spf(%s, %s, %s) = %i (%s)",
                   cd->num, cd->state, queue_id, td->num,
                   host, config.spf_heloname,
                   cd->sender, spf_ret, SPF_strresult(spf_ret));
          // TODO: make this configurable
          // (I'm not sure if SRS "MAIL FROM:" sender adress format can
          // cause some problems/mail rejection, so right now I'm taking
          // conservative approach for SRS "MAIL FROM:" rewriting)
          //if (!(status == SPF_RESULT_PASS || status == SPF_RESULT_NEUTRAL))
          if (spf_result == SPF_RESULT_FAIL || spf_result == SPF_RESULT_SOFTFAIL)
            fix_envfrom = 1;
        } else {
          if (config.verbose)
            syslog(LOG_DEBUG, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): spf(%s, %s, %s) NULL response?!",
                   cd->num, cd->state, queue_id, td->num, host,
                   config.spf_heloname, cd->sender);
        }

        break;
      }

      if (spf_ret != SPF_E_SUCCESS) {
        syslog(LOG_NOTICE, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): libspf2 error %i (%s)",
               cd->num, cd->state, queue_id, td->num, spf_ret, SPF_strerror(spf_ret));
      }

      // free SPF resources
      if (spf_response)
        SPF_response_free(spf_response);
      if (spf_request)
        SPF_request_free(spf_request);

    }
  }

  // debug log gathered data...
  if (config.verbose) {
    int i = 0;
    syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): forward = %i, fix_envfrom = %i%s",
           cd->num, cd->state, queue_id, config.forward, fix_envfrom,
           cd->recip_remote ? " (message has remote recipient)" : "");
    for (i = 0; cd->recip && cd->recip[i]; i++);
    syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): reverse = %i, rewrite_count = %i",
           cd->num, cd->state, queue_id, config.reverse, i);
    syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): sender = %s%s",
           cd->num, cd->state, queue_id, cd->sender,
           is_local_addr(cd->sender) ? " (local)" : "");
    for (i = 0; cd->recip && cd->recip[i]; i++)
      syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): recip = %s%s",
             cd->num, cd->state, queue_id, cd->recip[i],
             is_local_addr(cd->recip[i]) ? " (local)" : "");
  }

  // now, do some SRS magic...
  if (fix_envfrom || (config.reverse && cd->recip)) {
    int i;
    struct srs_milter_thread_data* td;

    td = (struct srs_milter_thread_data*) pthread_getspecific(key);
    if (!td) {
      syslog(LOG_ERR, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): can't get thread data!? (SRS)",
             cd->num, cd->state, queue_id);
      cd->state |= SS_STATE_INVALID_MSG;
      return SMFIS_CONTINUE;
    }

    if (!td->srs) { // initialize & configure SRS
      if (config.verbose)
        syslog(LOG_DEBUG, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): srs_new",
               cd->num, cd->state, queue_id, td->num);

      td->srs = srs_new();
      if (!td->srs) {
        syslog(LOG_ERR, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): can't initialize SRS",
               cd->num, cd->state, queue_id, td->num);
        cd->state |= SS_STATE_INVALID_MSG;
        return SMFIS_CONTINUE;
      }

      if (config.srs_alwaysrewrite > 0)
        srs_set_alwaysrewrite(td->srs, config.srs_alwaysrewrite);
      if (config.srs_hashlength > 0)
        srs_set_hashlength(td->srs, config.srs_hashlength);
      if (config.srs_hashmin > 0)
        srs_set_hashmin(td->srs, config.srs_hashmin);
      if (config.srs_maxage > 0)
        srs_set_maxage(td->srs, config.srs_maxage);
      if (config.srs_separator != 0)
        srs_set_separator(td->srs, config.srs_separator);
      for (i = 0; config.srs_secrets && config.srs_secrets[i]; i++)
        srs_add_secret(td->srs, config.srs_secrets[i]);
    }

    int srs_res;
    char *out = NULL;

    if (fix_envfrom) {
      // modify MAIL FROM: address to SRS format
      if ((srs_res = srs_forward_alloc(td->srs, &out, cd->sender, config.srs_domain)) == SRS_SUCCESS) {
        if (smfi_chgfrom(ctx, out, NULL) != MI_SUCCESS) {
          syslog(LOG_ERR, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): smfi_chgfrom(ctx, %s, NULL) failed",
                 cd->num, cd->state, queue_id, td->num, out);
        } else {
          if (config.verbose)
            syslog(LOG_DEBUG, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): smfi_chgfrom(ctx, %s, NULL) OK",
                   cd->num, cd->state, queue_id, td->num, out);
        }
      } else {
        syslog(LOG_ERR, "conn# %d[%i][%s][%i] - xxfi_srs_milter_eom(): srs_forward_alloc(srs, out, %s, %s) failed: %i (%s)",
               cd->num, cd->state, queue_id, td->num, cd->sender, config.srs_domain, srs_res, srs_strerror(srs_res));
      }

      if (out)
        free(out);
    }

    if (config.reverse && cd->recip) {
      // modify RCPT TO: by removing SRS format
      for (i = 0; cd->recip[i]; i++) {
        if ((srs_res = srs_reverse_alloc(td->srs, &out, cd->recip[i])) == SRS_SUCCESS) {
          if (smfi_delrcpt(ctx, cd->recip[i]) != MI_SUCCESS) {
            syslog(LOG_ERR, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): smfi_delrcpt(ctx, %s) failed",
                   cd->num, cd->state, queue_id, cd->recip[i]);
          } else if (smfi_addrcpt(ctx, out) != MI_SUCCESS) {
            syslog(LOG_ERR, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): smfi_addrcpt(ctx, %s) failed",
                   cd->num, cd->state, queue_id, out);
          } else {
            if (config.verbose)
              syslog(LOG_DEBUG, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): smfi_{del,add}rcpt(%s, %s) OK",
                     cd->num, cd->state, queue_id, cd->recip[i], out);
          }
        } else {
          syslog(LOG_ERR, "conn# %d[%i][%s] - xxfi_srs_milter_eom(): srs_reverse_alloc(srs, out, %s) failed: %i (%s)",
                 cd->num, cd->state, queue_id, cd->recip[i], srs_res, srs_strerror(srs_res));
        }

        if (out)
          free(out);
      }
    }
  }

  return SMFIS_CONTINUE;
}



// https://www.milter.org/developers/api/xxfi_close
static sfsistat
xxfi_srs_milter_close(SMFICTX* ctx) {
  struct srs_milter_connection_data* cd =
          (struct srs_milter_connection_data*) smfi_getpriv(ctx);

  if (config.verbose) {
    if (cd)
      syslog(LOG_DEBUG, "conn# %d[%i] - xxfi_srs_milter_close()",
             cd->num, cd->state);
    else
      syslog(LOG_DEBUG, "conn# ?[not_connected] - xxfi_srs_milter_close()");
  }

  if (!cd)
    return SMFIS_CONTINUE;

  smfi_setpriv(ctx, NULL);

  if (cd->sender)
    free(cd->sender);

  if (cd->envfromargv) {
    int i;
    for (i = 0; cd->envfromargv[i]; i++)
      free(cd->envfromargv[i]);
    free(cd->envfromargv);
  }

  if (cd->recip) {
    int i;
    for (i = 0; cd->recip[i]; i++)
      free(cd->recip[i]);
    free(cd->recip);
  }

  free(cd);

  return SMFIS_CONTINUE;
}




static struct smfiDesc smfilter = {
  SRS_MILTER_NAME,              /* filter name */
  SMFI_VERSION,                 /* version code -- do not change */
  SMFIF_CHGFROM | SMFIF_ADDRCPT | SMFIF_DELRCPT, /* flags */
  xxfi_srs_milter_connect,      /* connection info filter */
  NULL,                         /* SMTP HELO command filter */
  xxfi_srs_milter_envfrom,      /* envelope sender filter */
  xxfi_srs_milter_envrcpt,      /* envelope recipient filter */
  NULL,                         /* header filter */
  NULL,                         /* end of header */
  NULL,                         /* body block filter */
  xxfi_srs_milter_eom,          /* end of message */
  NULL,                         /* message aborted */
  xxfi_srs_milter_close         /* connection cleanup */
};




void daemonize() {
  pid_t pid, sid;

  /* Fork off the parent process */
  pid = fork();
  if (pid < 0) {
    syslog(LOG_ERR, "forking daemon process failed: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }
  /* If we got a good PID, then
     we can exit the parent process. */
  if (pid > 0) {
    syslog(LOG_ERR, "exiting parent process");
    exit(EXIT_SUCCESS);
  }

  /* Change the file mode mask */
  umask(0);

  /* Open any logs here */

  /* Create a new SID for the child process */
  sid = setsid();
  if (sid < 0) {
    syslog(LOG_ERR, "can't create new SID: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* Change the current working directory */
  if ((chdir("/")) < 0) {
    syslog(LOG_ERR, "can't chagne working directory: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* Close out the standard file descriptors */
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
}




void usage(char *argv0) {
  printf("SRS milter (version %s)\n", SRS_MILTER_VERSION);
  printf("usage:\n");
  printf("  %s [--forward] [--reverse] \\\n", argv0);
  printf("    --socket unix:/var/run/srs-milter.sock \\\n");
  printf("    --srs-domain=example.com --srs-secret-file=secret-file \\\n");
  printf("    [--local-domain=example.com] [--local-domain=.example.com ...]\n");
  printf("\n");
  printf("options:\n");
  printf("  -h, --help\n");
  printf("      this help message\n");
  printf("  -v, --verbose\n");
  printf("      verbose output\n");
  printf("  -d, --daemon\n");
  printf("      daemonize this process\n");
  printf("  -C, --config\n");
  printf("      configuration file (use long variant of command line options)\n");
  printf("  -P, --pidfile\n");
  printf("      filename where to store process PID\n");
  printf("  -s, --socket\n");
  printf("      {unix|local}:/path/to/file -- a named pipe.\n");
  printf("      inet:port@{hostname|ip-address} -- an IPV4 socket.\n");
  printf("      inet6:port@{hostname|ip-address} -- an IPV6 socket.\n");
  printf("  -t, --timeout\n");
  printf("      milter timeout\n");
  printf("  -f, --forward\n");
  printf("      SRS encode the envelope sender of non-local-destined mail\n");
  printf("  -r, --reverse\n");
  printf("      SRS decode any envelope recipients of local SRS addresses\n");
  printf("  -m, --local-domain\n");
  printf("      all local mail domains for that we accept mail\n");
  printf("      starting domain name with \".\" match also all subdomains\n");
  printf("  -o, --srs-domain\n");
  printf("      our SRS domain name\n");
  printf("  -c, --srs-secret\n");
  printf("      secret string for SRS hashing algorithm\n");
  printf("      WARNING: this is NOT secure, it is recommended to use --srs-secret-file\n");
  printf("               instead to ensure secrets are not visible in process listings\n");
  printf("  -C, --srs-secret-file\n");
  printf("      file containing secrets for SRS hashing algorithm\n");
  printf("  -w, --srs-alwaysrewrite\n");
  printf("  -g, --srs-hashlength\n");
  printf("  -i, --srs-hashmin\n");
  printf("  -x, --srs-maxage\n");
  printf("  -e, --srs-separator\n");
  printf("      SRS address separator, must be one of '+' '-' '=' (default: libsrs2 default)\n");
  printf("  -k, --spf-check\n");
  printf("      use SRS only when sender's SPF record will (soft)fail us\n");
  printf("  -l, --spf-heloname\n");
  printf("      use this heloname for SPF checks (default: gethostname())\n");
  printf("  -a, --spf-address\n");
  printf("      use this address for SPF checks (default: gethostaddr())\n");
  printf("\n");
}




int main(int argc, char* argv[]) {
  int c, i;
  int daemon = 0;
  FILE *f;

  static struct option long_options[] = {
    /* These options set a flag. */
//    {"verbose", no_argument,       &verbose_flag, 1},
//    {"brief",   no_argument,       &verbose_flag, 0},
    /* These options don't set a flag.
       We distinguish them by their indices. */
    {"help",                   no_argument,       0, 'h'},
    {"verbose",                no_argument,       0, 'v'},
    {"daemon",                 no_argument,       0, 'd'},
    {"config",                 required_argument, 0, 'C'},
    {"pidfile",                required_argument, 0, 'P'},
    {"socket",                 required_argument, 0, 's'},
    {"timeout",                required_argument, 0, 't'},
    {"forward",                no_argument,       0, 'f'},
    {"reverse",                no_argument,       0, 'r'},
    {"local-domain",           required_argument, 0, 'm'},
    {"spf-check",              no_argument,       0, 'k'},
    {"spf-heloname",           required_argument, 0, 'l'},
    {"spf-address",            required_argument, 0, 'a'},
    {"srs-domain",             required_argument, 0, 'o'},
    {"srs-always",             no_argument,       0, 'y'},
    {"srs-secret",             required_argument, 0, 'c'},
    {"srs-secret-file",        required_argument, 0, 'C'},
    {"srs-alwaysrewrite",      no_argument,       0, 'w'},
    {"srs-hashlength",         required_argument, 0, 'g'},
    {"srs-hashmin",            required_argument, 0, 'i'},
    {"srs-maxage",             required_argument, 0, 'x'},
    {"srs-separator",          required_argument, 0, 'e'},
    {0, 0, 0, 0}
  };

  /* reset configuration */
  memset(&config, 0, sizeof(config_t));

  /* find config file in command line arguments */
  while (1) {
    /* getopt_long stores the option index here. */
    int option_index = 0;

    c = getopt_long(argc, argv, "hdvP:s:t:f:r:mk:t:l:a:o:yc:C:wg:i:x:e:",
                    long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1)
      break;

    if (c != 'C')
      continue;

    if (srs_milter_load_config(optarg) != 0)
      exit(EXIT_FAILURE);
  }

  /* reset getopt */
  optind = 1;

  while (1) {
    /* getopt_long stores the option index here. */
    int option_index = 0;

    c = getopt_long(argc, argv, "hdvP:s:t:f:r:mk:t:l:a:o:yc:C:wg:i:x:e:",
                    long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1)
      break;

    switch (c) {
      case 0:
        /* If this option set a flag, do nothing else now. */
        if (long_options[option_index].flag != 0)
          break;
        printf("option %s", long_options[option_index].name);
        if (optarg)
          printf(" with arg %s", optarg);
        printf("\n");
        break;

      case 'h':
        usage(argv[0]);
        exit(EXIT_SUCCESS);
        break;

      case 'd':
        daemon = 1;
        break;

      case 'v':
        srs_milter_configure("verbose", NULL);
        break;

      case 'C':
        break;

      case 'P':
        srs_milter_configure("pidfile", optarg);
        break;

      case 's':
        srs_milter_configure("socket", optarg);
        break;

      case 't':
        srs_milter_configure("timeout", optarg);
        break;

      case 'f':
        srs_milter_configure("forward", NULL);
        break;

      case 'r':
        srs_milter_configure("reverse", NULL);
        break;

      case 'm':
        srs_milter_configure("local-domain", optarg);
        break;

      case 'k':
        srs_milter_configure("spf_check", NULL);
        break;

      case 'c':
        srs_milter_configure("spf_secret", optarg);
        break;

      case 'l':
        srs_milter_configure("spf_heloname", optarg);
        break;

      case 'a':
        srs_milter_configure("spf_address", optarg);
        break;

      case 'o':
        srs_milter_configure("srs_domain", optarg);
        break;

      case 'w':
        srs_milter_configure("srs_alwaysrewrite", NULL);
        break;

      case 'g':
        srs_milter_configure("srs_hashlength", optarg);
        break;

      case 'i':
        srs_milter_configure("srs_hashmin", optarg);
        break;

      case 'x':
        srs_milter_configure("srs_maxage", optarg);
        break;

      case 'e':
        srs_milter_configure("srs_separator", optarg);
        break;

      case '?':
        /* getopt_long already printed an error message. */
        break;

      default:
        abort ();
    }
  }

  /* Print any remaining command line arguments (not options). */
  if (optind < argc) {
    printf ("non-option ARGV-elements: ");
    while (optind < argc)
      printf ("%s ", argv[optind++]);
    putchar ('\n');
  }

  if (pthread_key_create(&key, &srs_milter_thread_data_destructor)) {
      fprintf(stderr, "pthread_key_create failed");
      exit(EXIT_FAILURE);
  }

  openlog(SRS_MILTER_NAME, LOG_PID, LOG_MAIL);
  {
    int i;
    char args[1024] = "";
    for (i = 1; i < argc; i++) {
      strncat(args, " ", 1023);
      strncat(args, argv[i], 1023);
    }
    syslog(LOG_NOTICE, "Starting %s v%s (args:%s)", SRS_MILTER_NAME, SRS_MILTER_VERSION, args);

    SPF_error_handler = SPF_error_syslog;
    SPF_warning_handler = SPF_warning_syslog;
    SPF_info_handler = SPF_info_syslog;
    SPF_debug_handler = SPF_debug_syslog;
  }

  // print and validate configuration
  {
    // Milter library version
    unsigned int milter_major, milter_minor, milter_patch;
    smfi_version(&milter_major, &milter_minor, &milter_patch);
    syslog(LOG_DEBUG, "MILTER(%i, %i, %i)", milter_major, milter_minor, milter_patch);

    // SPF library version
    int spf_major, spf_minor, spf_patch;
    SPF_get_lib_version(&spf_major, &spf_minor, &spf_patch);
    syslog(LOG_DEBUG, "SPF(%i, %i, %i)", spf_major, spf_minor, spf_patch);

    // SRS library version
    // ???

    // validate configuration
    if (!config.forward && !config.reverse) {
      usage(argv[0]);
      fprintf(stderr, "ERROR: use forward or reverse (or both)\n");
      exit(EXIT_FAILURE);
    }

    if (!config.socket) {
      usage(argv[0]);
      fprintf(stderr, "ERROR: missing socket configuration\n");
      exit(EXIT_FAILURE);
    }

    if (!config.srs_domain || strlen(config.srs_domain) == 0) {
      usage(argv[0]);
      fprintf(stderr, "ERROR: invalid or missing srs domain configuration\n");
      exit(EXIT_FAILURE);
    }

    if (config.spf_check) {

      if (!config.spf_heloname) {
        config.spf_heloname = (char *) malloc(64);
        gethostname(config.spf_heloname, 63);
      }

      if (!config.spf_address_defined) {
        // get local address
        struct ifaddrs *ifAddrStruct = NULL;
        struct ifaddrs *ifa = NULL;

        getifaddrs(&ifAddrStruct);

        for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
         if (!ifa->ifa_addr)
            continue;
          if(ifa->ifa_flags & IFF_LOOPBACK)
            continue;
          if(ifa->ifa_flags & IFF_POINTOPOINT)
            continue;
          if (ifa->ifa_addr->sa_family == AF_INET) { // check it is IP4
            memcpy(&config.spf_address.in, ifa->ifa_addr, sizeof(struct sockaddr_in));
            break;
//          } else if (ifa->ifa_addr->sa_family == AF_INET6) { // check it is IP6
//            memcpy(&config.spf_address.in6, ifa->ifa_addr, sizeof(struct sockaddr_in6));
          }
        }

        if (ifAddrStruct!=NULL)
          freeifaddrs(ifAddrStruct);
      }
    }

    if (!config.srs_secrets || !config.srs_secrets[0]) {
      usage(argv[0]);
      fprintf(stderr, "ERROR: missing srs-secrets configuration\n");
      exit(EXIT_FAILURE);
    }

    srs_t *srs = srs_new();
    int srs_res = SRS_SUCCESS;

    while (1) {
      if (config.srs_alwaysrewrite > 0)
        if ((srs_res = srs_set_alwaysrewrite(srs, config.srs_alwaysrewrite)) != SRS_SUCCESS)
          break;
      if (config.srs_hashlength > 0)
        if ((srs_res = srs_set_hashlength(srs, config.srs_hashlength)) != SRS_SUCCESS)
          break;
      if (config.srs_hashmin > 0)
        if ((srs_res = srs_set_hashmin(srs, config.srs_hashmin)) != SRS_SUCCESS)
          break;
      if (config.srs_maxage > 0)
        if ((srs_res = srs_set_maxage(srs, config.srs_maxage)) != SRS_SUCCESS)
          break;
      if (config.srs_separator != 0)
        if ((srs_res = srs_set_separator(srs, config.srs_separator)) != SRS_SUCCESS)
          break;
      for (i = 0; config.srs_secrets && config.srs_secrets[i]; i++)
        if ((srs_res = srs_add_secret(srs, config.srs_secrets[i])) != SRS_SUCCESS)
          break;
      break;
    }
    if (srs_res != SRS_SUCCESS) {
      usage(argv[0]);
      fprintf(stderr, "ERROR: failure while setting SRS configuration: %i (%s)\n", srs_res, srs_strerror(srs_res));
      exit(EXIT_FAILURE);
    }

    srs_free(srs);

    // print configuration
    if (config.verbose) {
      if (config.pidfile)
        syslog(LOG_DEBUG, "config pidfile: %s", config.pidfile);
      if (config.forward)
        syslog(LOG_DEBUG, "config forward: %i", config.forward);
      if (config.reverse)
        syslog(LOG_DEBUG, "config reverse: %i", config.reverse);
      if (config.socket)
        syslog(LOG_DEBUG, "config socket: %s", config.socket);
      if (config.timeout)
        syslog(LOG_DEBUG, "config timeout: %i", config.timeout);
      for (i = 0; config.domains && config.domains[i]; i++)
        syslog(LOG_DEBUG, "config local_domain: %s", config.domains[i]);
      if (config.srs_domain)
        syslog(LOG_DEBUG, "config srs_domain: %s", config.srs_domain);
      for (i = 0; config.srs_secrets && config.srs_secrets[i]; i++)
        syslog(LOG_DEBUG, "config srs_secret: %s", config.srs_secrets[i]);
      if (config.srs_alwaysrewrite > 0)
        syslog(LOG_DEBUG, "config srs_alwaysrewrite: %i", config.srs_alwaysrewrite);
      if (config.srs_hashlength > 0)
        syslog(LOG_DEBUG, "config srs_hashlength: %i", config.srs_hashlength);
      if (config.srs_hashmin > 0)
        syslog(LOG_DEBUG, "config srs_hashmin: %i", config.srs_hashmin);
      if (config.srs_maxage > 0)
        syslog(LOG_DEBUG, "config srs_maxage: %i", config.srs_maxage);
      if (config.srs_separator != 0)
        syslog(LOG_DEBUG, "config srs_separator: %c", config.srs_separator);
      syslog(LOG_DEBUG, "config spf_check: %i", config.spf_check);
      if (config.spf_check) {
        if (config.spf_heloname)
          syslog(LOG_DEBUG, "config spf_heloname: %s", config.spf_heloname);
        if (config.spf_address.in.sin_family == AF_INET) {
          char host[INET_ADDRSTRLEN+1];
          inet_ntop(AF_INET, &config.spf_address.in.sin_addr, host, INET_ADDRSTRLEN);
          syslog(LOG_DEBUG, "config spf_address: %s (IP)", host);
        } else {
          char host[INET_ADDRSTRLEN+1];
          inet_ntop(AF_INET6, &config.spf_address.in6.sin6_addr, host, INET_ADDRSTRLEN);
          syslog(LOG_DEBUG, "config spf_address: %s (IPv6)", host);
        }
      }
    }
  }

  {
    pid_t ppid = getpid();
    if (daemon) {
      daemonize();
      syslog(LOG_NOTICE, "daemonized PID %i", (int) ppid);
    }
  }

  if (config.pidfile) {
    f = fopen(config.pidfile, "w");
    if (!f) {
      fprintf(stderr, "ERROR: can't open PID file %s\n", config.pidfile);
      exit(EXIT_FAILURE);
    }
    fprintf(f, "%i", (int) getpid());
    fclose(f);
  }

  smfi_setconn(config.socket);
  if (smfi_register(smfilter) == MI_FAILURE) {
    fprintf(stderr, "%s: register failed\n", SRS_MILTER_NAME);
    exit(EXIT_FAILURE);
  }
  if (config.timeout) {
    if (smfi_settimeout(config.timeout) == MI_FAILURE) {
      fprintf(stderr, "%s: can't set milter timeout to %i\n", SRS_MILTER_NAME, config.timeout);
      exit(EXIT_FAILURE);
    }
  }
  if (smfi_main() == MI_FAILURE) {
    fprintf(stderr, "%s: milter failed\n", SRS_MILTER_NAME);
    exit(EXIT_FAILURE);
  }

  // Free the secrets
  {
    char **s = config.srs_secrets;
    do {
      free(*s);
    } while (*++s != NULL);
  }
  // Free the secrets
  {
    char **s = config.domains;
    do {
      free(*s);
    } while (*++s != NULL);
  }

  syslog(LOG_INFO, "exitting");

  pthread_key_delete(key);

  closelog();

  exit(EXIT_SUCCESS);
}
