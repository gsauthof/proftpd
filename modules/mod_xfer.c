/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 *  
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * Data transfer module for ProFTPD
 * $Id: mod_xfer.c,v 1.2 1998-11-01 19:08:25 flood Exp $
 */

/* History Log:
 *
 * 4/24/97 0.99.0pl1
 *   _translate_ascii was returning a buffer larger than the max buffer
 *   size causing memory overrun and all sorts of neat corruption.
 *   Status: Stomped
 *
 */

#include "conf.h"
#ifdef HAVE_REGEX_H
#include <regex.h>
#endif

extern module auth_module;
extern pid_t mpid;

/* From the auth module */
char *auth_map_uid(int);
char *auth_map_gid(int);

void xfer_abort(IOFILE*,int);

/* Variables for this module */
static fsdir_t *retr_file = NULL;
static fsdir_t *stor_file = NULL;
static int stor_fd;
static int retr_fd;

module xfer_module;

static void _log_transfer(char direction)
{
  struct timeval end_time;
  char *fullpath;

  gettimeofday(&end_time,NULL);

  end_time.tv_sec -= session.xfer.start_time.tv_sec;
  if(end_time.tv_usec >= session.xfer.start_time.tv_usec)
    end_time.tv_usec -= session.xfer.start_time.tv_usec;
  else {
    end_time.tv_usec = 1000000L - (session.xfer.start_time.tv_usec -
                       end_time.tv_usec);
    end_time.tv_sec--;
  }

  fullpath = dir_abs_path(session.xfer.p,session.xfer.path,TRUE);

  if((session.flags & SF_ANON) != 0) {
    log_xfer(end_time.tv_sec,session.c->remote_name,session.xfer.total_bytes,
             fullpath,(session.flags & SF_ASCII ? 'a' : 'b'),
             direction,'a',session.anon_user);
  } else
    log_xfer(end_time.tv_sec,session.c->remote_name,session.xfer.total_bytes,
             fullpath,(session.flags & SF_ASCII ? 'a' : 'b'),
             direction,'r',session.user);

  log_debug(DEBUG1,"Transfer completed: %d bytes in %d.%02d seconds.",
                 session.xfer.total_bytes,end_time.tv_sec,
                 (end_time.tv_usec / 10000));

  data_cleanup();
}

static
void _stor_done()
{
  fs_close(stor_file,stor_fd);
  stor_file = NULL;

  if(session.fsgid && session.xfer.path) {
    struct stat sbuf;

    fs_stat(session.xfer.path,&sbuf);
    if(chown(session.xfer.path,(uid_t)-1,(gid_t)session.fsgid) == -1)
      log_pri(LOG_WARNING,"chown(%s) failed: %s",
              session.xfer.path,strerror(errno));
    else
      fs_chmod(session.xfer.path,sbuf.st_mode);
  }
}

static
void _retr_done()
{
  fs_close(retr_file,retr_fd);
  retr_file = NULL;
}

static
void _stor_abort()
{
  fs_close(stor_file,stor_fd);
  stor_file = NULL;
  if(session.xfer.path)
    unlink(session.xfer.path);
}

static
void _retr_abort()
{
  /* Isn't necessary to send anything here, just cleanup */
  fs_close(retr_file,retr_fd);
  retr_file = NULL;
}

/* cmd_pre_stor is a PRE_CMD handler which checks security, etc, and
 * places the full filename to receive in cmd->private [note that we CANNOT
 * use cmd->tmp_pool for this, as tmp_pool only lasts for the duration
 * of this function.
 */

MODRET pre_cmd_stor(cmd_rec *cmd)
{
  char *dir;
  mode_t fmode;
  privdata_t *p;

  if(cmd->argc < 2) {
    add_response_err(R_500,"'%s' not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  dir = dir_best_path(cmd->tmp_pool,cmd->arg);

  if(!dir || !dir_check(cmd->tmp_pool,cmd->argv[0],cmd->group,dir,NULL)) {
    add_response_err(R_550,"%s: %s",cmd->arg,strerror(errno));
    return ERROR(cmd);
  }

  fmode = file_mode(dir);

  if(fmode && !session.xfer.stor_append && 
	       get_param_int(CURRENT_CONF,"AllowOverwrite",FALSE) != 1) {
    add_response_err(R_550,"%s: Overwrite permission denied",cmd->arg);
    return ERROR(cmd);
  }

  if(fmode && !S_ISREG(fmode)) {
    add_response_err(R_553,"%s: Not a regular file",cmd->arg);
    return ERROR(cmd);
  }

  /* If restarting, check permissions on this directory, if
   * AllowStoreRestart is set, permit it
   */

  if(fmode && (session.restart_pos || session.xfer.stor_append) &&
     get_param_int(CURRENT_CONF,"AllowStoreRestart",FALSE) != TRUE) {
    add_response_err(R_451,"%s: Append/Restart not permitted, try again.",
                  cmd->arg);
    session.restart_pos = 0L;
    session.xfer.stor_append = 0;
    return ERROR(cmd);
  }

  /* otherwise everthing is good */
  p = mod_privdata_alloc(cmd,"stor_filename",strlen(dir)+1);
  strcpy(p->value.str_val,dir);

  return HANDLED(cmd);
}

/* cmd_pre_appe is the PRE_CMD handler for the APPEnd command, which
 * simply sets stor_append and calls pre_cmd_stor
 */

MODRET pre_cmd_appe(cmd_rec *cmd)
{
  session.xfer.stor_append = 1;
  session.restart_pos = 0L;
  
  return pre_cmd_stor(cmd);
}

MODRET cmd_stor(cmd_rec *cmd)
{
  char *dir;
  char *lbuf;
  int bufsize,len;
  unsigned long respos = 0;
  privdata_t *p;
#if defined(HAVE_REGEX_H) && defined(HAVE_REGCOMP)
  regex_t *preg;
  int ret;
#endif

  p = mod_privdata_find(cmd,"stor_filename",NULL);

  if(!p) {
    add_response_err(R_550,"%s: internal error, what happened to cmd_pre_retr?!?",cmd->arg);
    return ERROR(cmd);
  }

#if defined(HAVE_REGEX_H) && defined(HAVE_REGCOMP)
  preg = (regex_t*)get_param_ptr(TOPLEVEL_CONF,"PathAllowFilter",FALSE);

  if(preg && ((ret = regexec(preg,cmd->arg,0,NULL,0)) != 0)) {
    char errmsg[200];
    regerror(ret,preg,errmsg,200);
    log_debug(DEBUG2,"'%s' didn't pass regex: %s",cmd->arg,errmsg);
    add_response_err(R_550,"%s: Forbidden filename", cmd->arg);
    return ERROR(cmd);
  }

  preg = (regex_t*)get_param_ptr(TOPLEVEL_CONF,"PathDenyFilter",FALSE);

  if(preg && ((ret = regexec(preg,cmd->arg,0,NULL,0)) == 0)) {
    add_response_err(R_550,"%s: Forbidden filename", cmd->arg);
    return ERROR(cmd);
  }
#endif

  dir = p->value.str_val;

  if(session.xfer.stor_append)
	  stor_file = fs_open(dir,O_WRONLY|O_CREAT|O_APPEND,&stor_fd);
  else
	  stor_file = fs_open(dir,O_WRONLY|(session.restart_pos ? 0 : O_TRUNC|O_CREAT),&stor_fd);

  if(stor_file && session.restart_pos) {
    if(fs_lseek(stor_file,stor_fd,session.restart_pos,SEEK_SET) == -1) {
      int _errno = errno;
      fs_close(stor_file,stor_fd);
      errno = _errno;
      stor_file = NULL;
    }

    respos = session.restart_pos;
    session.restart_pos = 0L;
  }

  if(!stor_file) {
    add_response_err(R_550,"%s: %s",cmd->arg,strerror(errno));
    return ERROR(cmd);
  } else {
    /* perform the actual transfer now */
    data_init(cmd->arg,IO_READ);

    session.xfer.path = pstrdup(session.xfer.p,dir);
    session.xfer.file_size = respos;

    if(data_open(cmd->arg,NULL,IO_READ,0) < 0) {
      data_abort(0,TRUE);
      return HANDLED(cmd);
    }

    bufsize = (main_server->tcp_rwin > 0 ? main_server->tcp_rwin : 1024);
    lbuf = (char*)palloc(cmd->tmp_pool,bufsize);

    while((len = data_xfer(lbuf,bufsize)) > 0) {
      if(XFER_ABORTED)
        break;

      len = fs_write(stor_file,stor_fd,lbuf,len);
      if(len < 0) {
        int s_errno = errno;
        _stor_abort();
        data_abort(s_errno,FALSE);
        return ERROR(cmd);
      }
    }

    if(XFER_ABORTED) {
      _stor_abort();
      data_abort(0,0);
      return ERROR(cmd);
    } else if(len < 0) {
      _stor_abort();
      data_abort(session.d->inf->xerrno,FALSE);
      return ERROR(cmd);
    } else {
      _stor_done();
      data_close(FALSE);
    }
  }
  return HANDLED(cmd);
}

MODRET cmd_rest(cmd_rec *cmd)
{
  long int pos;
  char *endp;

  if(cmd->argc != 2) {
    add_response_err(R_500,"'%s': command not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  pos = strtol(cmd->argv[1],&endp,10);
  if((endp && *endp) || pos < 0) {
    add_response_err(R_501,"REST requires a value greater than or equal to 0.");
    return ERROR(cmd);
  }

  session.restart_pos = pos;
  add_response(R_350,"Restarting at %ld. Send STORE or RETRIEVE to initiate transfer.",
                pos);
  return HANDLED(cmd);
}

/* cmd_pre_retr is a PRE_CMD handler which checks security, etc, and
 * places the full filename to send in cmd->private [note that we CANNOT
 * use cmd->tmp_pool for this, as tmp_pool only lasts for the duration
 * of this function.
 */

MODRET pre_cmd_retr(cmd_rec *cmd)
{
  char *dir;
  mode_t fmode;
  privdata_t *p;

  if(cmd->argc < 2) {
    add_response_err(R_500,"'%s' not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  dir = dir_realpath(cmd->tmp_pool,cmd->arg);

  if(!dir || !dir_check(cmd->tmp_pool,cmd->argv[0],cmd->group,dir,NULL)) {
    add_response_err(R_550,"%s: %s",cmd->arg,strerror(errno));
    return ERROR(cmd);
  }


  fmode = file_mode(dir);

  if(!S_ISREG(fmode)) {
    if(!fmode)
      add_response_err(R_550,"%s: %s",cmd->arg,strerror(errno));
    else
      add_response_err(R_553,"%s: Not a regular file",cmd->arg);
    return ERROR(cmd);
  }

  /* If restart is on, check to see if AllowRestartRetrieve
   * is off, in which case we disallow the transfer and
   * clear restart_pos
   */

  if(session.restart_pos &&
     get_param_int(CURRENT_CONF,"AllowRetrieveRestart",FALSE) == 0) {
    add_response_err(R_451,"%s: Restart not permitted, try again.",
                  cmd->arg);
    session.restart_pos = 0L;
    return ERROR(cmd);
  }

  /* otherwise everthing is good */
  p = mod_privdata_alloc(cmd,"retr_filename",strlen(dir)+1);
  strcpy(p->value.str_val,dir);
  return HANDLED(cmd);
}

MODRET cmd_retr(cmd_rec *cmd)
{
  char *dir;
  struct stat sbuf;
  char *lbuf;
  int bufsize,len;
  unsigned long respos = 0,cnt = 0,cnt_steps = 0,cnt_next = 0;
  privdata_t *p;

  p = mod_privdata_find(cmd,"retr_filename",NULL);

  if(!p) {
    add_response_err(R_550,"%s: internal error, what happened to cmd_pre_retr?!?",cmd->arg);
    return ERROR(cmd);
  }

  dir = p->value.str_val;
  retr_file = fs_open(dir,O_RDONLY,&retr_fd);

  if(session.restart_pos) {
    if(fs_lseek(retr_file,retr_fd,session.restart_pos,SEEK_SET) == -1) {
      int _errno = errno;
      fs_close(retr_file,retr_fd);
      errno = _errno;
      retr_file = NULL;
    }

    respos = session.restart_pos;
    session.restart_pos = 0L;
  }

  if(!retr_file || fs_stat(dir,&sbuf) == -1) {
    add_response_err(R_550,"%s: %s",cmd->arg,strerror(errno));
    return ERROR(cmd);
  } else {
    /* send the data */
    data_init(cmd->arg,IO_WRITE);

    session.xfer.path = pstrdup(session.xfer.p,dir);
    session.xfer.file_size = (unsigned long)sbuf.st_size;
    cnt_steps = session.xfer.file_size / 100;
    if(cnt_steps == 0)
      cnt_steps = 1;

    if(data_open(cmd->arg,NULL,IO_WRITE,sbuf.st_size - respos) < 0) {
      data_abort(0,TRUE);
      return ERROR(cmd);
    }

    bufsize = (main_server->tcp_swin > 0 ? main_server->tcp_swin : 1024);
    lbuf = (char*)palloc(cmd->tmp_pool,bufsize);

    cnt = respos;
    log_add_run(mpid,NULL,session.user,NULL,0,session.xfer.file_size,0,NULL);

    while((len = fs_read(retr_file,retr_fd,lbuf,bufsize)) > 0) {
      if(XFER_ABORTED)
        break;

      len = data_xfer(lbuf,len);
      if(len < 0) {
        _retr_abort();
        data_abort(session.d->outf->xerrno,FALSE);
        return ERROR(cmd);
      } else {
        cnt += len;

        if((cnt / cnt_steps) != cnt_next) {
          cnt_next = cnt / cnt_steps;
	  log_add_run(mpid,NULL,session.user,NULL,0,session.xfer.file_size,cnt,NULL);
        }
      }
    }

    if(XFER_ABORTED) {
      _retr_abort();
      data_abort(0,0);
      return ERROR(cmd);
    } else if(len < 0) {
      _retr_abort();
      data_abort(errno,FALSE);
      return ERROR(cmd);
    } else {
      _retr_done();
      data_close(FALSE);
    }
  }
  return HANDLED(cmd);
}

MODRET cmd_abor(cmd_rec *cmd)
{
  if(cmd->argc != 1) {
    add_response_err(R_500,"'%s' not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  if(session.flags & (SF_POST_ABORT|SF_ABORT)) {
    session.flags &= ~(SF_POST_ABORT|SF_ABORT);
    add_response(R_226,"Abort successful");
    return HANDLED(cmd);
  }

  add_response_err(R_500,"No command to abort.");
  return ERROR(cmd);
}

MODRET cmd_type(cmd_rec *cmd)
{
  if(cmd->argc != 2) {
    add_response_err(R_500,"'%s' not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  cmd->argv[1][0] = toupper(cmd->argv[1][0]);

  if(!strcmp(cmd->argv[1],"A"))
    session.flags |= SF_ASCII;
  else if(!strcmp(cmd->argv[1],"I"))
    session.flags &= (SF_ALL^SF_ASCII);
  else {
    add_response_err(R_500,"'%s' not understood.",get_full_cmd(cmd));
    return ERROR(cmd);
  }

  add_response(R_200,"Type set to %s.",cmd->argv[1]);
  return HANDLED(cmd);
}

MODRET log_stor(cmd_rec *cmd)
{
  _log_transfer('i');
  return DECLINED(cmd);
}

MODRET log_retr(cmd_rec *cmd)
{
  _log_transfer('o');
  return DECLINED(cmd);
}

static int _noxfer_timeout(CALLBACK_FRAME)
{
  if(session.flags & SF_XFER)
    return TRUE;			/* Transfer in progress, ignore timeout */

  send_response_async(R_421,
           "No Transfer Timeout (%d seconds): closing control connection.",
           TimeoutNoXfer);
  schedule(main_exit,0,(void*)LOG_NOTICE,"FTP no transfer time out, disconnected.",
           (void*)0,NULL);
  remove_timer(TIMER_IDLE,ANY_MODULE);
  remove_timer(TIMER_LOGIN,ANY_MODULE);
  return 0;
}

int xfer_init_child()
{
  /* Setup TimeoutNoXfer timer */
  if(TimeoutNoXfer)
    add_timer(TimeoutNoXfer,TIMER_NOXFER,&xfer_module,_noxfer_timeout);
  return 0;
}

cmdtable xfer_commands[] = {
  { CMD,     C_TYPE,	G_NONE,	 cmd_type,	TRUE,	FALSE, CL_MISC },
  { PRE_CMD, C_RETR,	G_READ,	 pre_cmd_retr,	TRUE,	FALSE },
  { CMD,     C_RETR,	G_READ,	 cmd_retr,	TRUE,	FALSE, CL_READ },
  { LOG_CMD, C_RETR,	G_NONE,	 log_retr,	FALSE,  FALSE },
  { PRE_CMD, C_STOR,	G_WRITE, pre_cmd_stor,	TRUE,	FALSE },
  { CMD,     C_STOR,	G_WRITE, cmd_stor,	TRUE,	FALSE, CL_WRITE },
  { LOG_CMD, C_STOR,    G_NONE,	 log_stor,	FALSE,  FALSE },
  { PRE_CMD, C_APPE,	G_WRITE, pre_cmd_appe,	TRUE,	FALSE },
  { CMD,     C_APPE,	G_WRITE, cmd_stor,	TRUE,	FALSE, CL_WRITE },
  { LOG_CMD, C_APPE,	G_NONE,  log_stor,	FALSE,  FALSE },
  { CMD,     C_ABOR,	G_NONE,	 cmd_abor,	TRUE,	TRUE,  CL_MISC  },
  { CMD,     C_REST,	G_NONE,	 cmd_rest,	TRUE,	FALSE, CL_MISC  },
  { 0,NULL }
};

module xfer_module = {
  NULL,NULL,				/* Always NULL */
  0x20,					/* API Version */
  "xfer",				/* Module name */
  NULL,					/* No config */
  xfer_commands,
  NULL,
  NULL,xfer_init_child
};
