/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mtev_defines.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <assert.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <zlib.h>

#include "mtev_conf.h"
#include "mtev_console.h"
#include "mtev_version.h"
#include "mtev_xml.h"
#include "mtev_hash.h"
#include "mtev_log.h"
#include "mtev_b64.h"
#include "mtev_watchdog.h"
#include "mtev_security.h"

const char *_mtev_branch = MTEV_BRANCH;
const char *_mtev_version = MTEV_VERSION;

/* tmp hash impl, replace this with something nice */
static mtev_log_stream_t xml_debug = NULL;
#define XML2LOG(log) do { \
  xmlSetGenericErrorFunc(log, mtev_conf_xml_error_func); \
  xmlSetStructuredErrorFunc(log, mtev_conf_xml_error_ext_func); \
} while(0)
#define XML2CONSOLE(ncct) do { \
  xmlSetGenericErrorFunc(ncct, mtev_conf_xml_console_error_func); \
  xmlSetStructuredErrorFunc(ncct, mtev_conf_xml_console_error_ext_func); \
} while(0)
static xmlDocPtr master_config = NULL;
static int config_include_cnt = -1;
static int backingstore_include_cnt = -1;

struct include_node_t{
  xmlNodePtr insertion_point;
  xmlNodePtr old_children;
  xmlDocPtr doc;
  xmlNodePtr root;
  int snippet;
  int ro;
  char path[255];
  int child_count;
  struct include_node_t *children;
};

typedef struct include_node_t include_node_t;

static include_node_t *config_include_nodes = NULL,
                      *backingstore_include_nodes = NULL;

typedef struct mtev_xml_userdata {
  char       *name;
  char       *path;
  u_int64_t   dirty_time;
  struct mtev_xml_userdata *freelist;
} mtev_xml_userdata_t;

static mtev_xml_userdata_t *backingstore_freelist = NULL;
static u_int64_t last_config_flush = 0;

static int default_is_stopword(const char *f) { return 0; }
static int (*is_stopnode_name)(const char *) = default_is_stopword; 

void mtev_override_console_stopword(int (*f)(const char *)) {
  is_stopnode_name = f;
}
#define is_stopnode(node) ((node) && is_stopnode_name((const char *)(node)->name))

static char *root_node_name = NULL;
static char master_config_file[PATH_MAX] = "";
static xmlXPathContextPtr xpath_ctxt = NULL;

/* coalesced writing allows internals to change the XML structure and mark
 * the tree dirty, but only write the config out once per second.
 */
static u_int32_t __coalesce_write = 0;

/* This is used to notice config changes and journal the config out
 * using a user-specified function.  It supports allowing multiple config
 * changed to coalesce so you don't write out 1000 changes in a few seconds.
 */
static u_int32_t __config_gen = 0;
static u_int32_t __config_coalesce = 0;
static u_int32_t __config_coalesce_time = 0;
static u_int64_t max_gen_count = 0;
void mtev_conf_coalesce_changes(u_int32_t seconds) {
  __config_coalesce_time = seconds;
}
void mtev_conf_request_write() {
  __coalesce_write = 1;
}
void mtev_conf_mark_changed() {
  /* increment the change counter -- in case anyone cares */
  __config_gen++;
  /* reset the coalesce counter.  It is decremented each second and
   * the journal function fires on a transition from 1 => 0
   */
  __config_coalesce = __config_coalesce_time;
}
struct recurrent_journaler {
  int (*journal_config)(void *);
  void *jc_closure;
};

void mtev_conf_write_section(mtev_conf_section_t node, int fd) {
  xmlOutputBufferPtr out;
  xmlCharEncodingHandlerPtr enc;

  enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
  out = xmlOutputBufferCreateFd(fd, enc);
  xmlNodeDumpOutput(out, master_config, node, 2, 0, "utf8");
  xmlOutputBufferClose(out);
  write(2, "\n", 1);
  xmlFree(enc);
}
void
write_out_include_files(include_node_t *include_nodes, int include_node_cnt) {
  int i;
  for(i=0; i<include_node_cnt; i++) {
    xmlOutputBufferPtr out;
    xmlCharEncodingHandlerPtr enc;
    mode_t mode = 0640;
    char filename[500];
    int len, fd;
    struct stat st;
    uid_t uid = 0;
    gid_t gid = 0;

    if(include_nodes[i].ro || !include_nodes[i].doc) {
      write_out_include_files(include_nodes[i].children, include_nodes[i].child_count);
      continue;
    }
    if(stat(include_nodes[i].path, &st) == 0) {
      mode = st.st_mode;
      uid = st.st_uid;
      gid = st.st_gid;
    }

    sprintf(filename, "%s.tmp", include_nodes[i].path);
    fd = open(filename, O_CREAT|O_TRUNC|O_WRONLY, mode);
    fchown(fd, uid, gid);

    enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
    out = xmlOutputBufferCreateFd(fd, enc);
    len = xmlSaveFormatFileTo(out, include_nodes[i].doc, "utf8", 1);
    if (len < 0) {
      mtevL(mtev_error, "couldn't write out %s\n", include_nodes[i].path);
      close(fd);
      continue;
    }
    close(fd);
    write_out_include_files(include_nodes[i].children, include_nodes[i].child_count);
    if(rename(filename, include_nodes[i].path) != 0) {
      mtevL(mtev_error, "Failed to replace file %s: %s\n", include_nodes[i].path, strerror(errno));
    }
  }
}

static void
mtev_xml_userdata_free(mtev_xml_userdata_t *n) {
  if(n->name) free(n->name);
  if(n->path) free(n->path);
}

static char *mtev_xml_ns = NULL;
void
mtev_conf_use_namespace(const char *ns) {
  if(mtev_xml_ns) free(mtev_xml_ns);
  mtev_xml_ns = strdup(ns);
}
void
mtev_conf_set_namespace(const char *ns) {
  xmlNsPtr nsptr;
  xmlNodePtr root;
  root = xmlDocGetRootElement(master_config);
  nsptr = xmlSearchNs(master_config, root, (xmlChar *)ns);
  if(!nsptr) {
    char url[128];
    snprintf(url, sizeof(url), "%s://module/%s", mtev_xml_ns ? mtev_xml_ns : "mtev", ns);
    xmlNewNs(root, (xmlChar *)url, (xmlChar *)ns);
  }
}

static int
mtev_conf_watch_config_and_journal(eventer_t e, int mask, void *closure,
                                   struct timeval *now) {
  struct recurrent_journaler *rj = closure;
  eventer_t newe;

  if(__config_coalesce == 1)
    rj->journal_config(rj->jc_closure);
  if(__config_coalesce > 0)
    __config_coalesce--;

  if(__coalesce_write) {
    mtev_conf_write_file(NULL);
    __coalesce_write = 0;
  }

  /* Schedule the same event to fire a second form now */
  newe = eventer_alloc();
  gettimeofday(&newe->whence, NULL);
  newe->whence.tv_sec += 1;
  newe->mask = EVENTER_TIMER;
  newe->callback = mtev_conf_watch_config_and_journal;
  newe->closure = closure;
  eventer_add(newe);
  return 0;
}

void
mtev_conf_watch_and_journal_watchdog(int (*f)(void *), void *c) {
  static int callbacknamed = 0;
  struct recurrent_journaler *rj;
  struct timeval __now;

  if(!callbacknamed) {
    callbacknamed = 1;
    eventer_name_callback("mtev_conf_watch_config_and_journal",
                          mtev_conf_watch_config_and_journal);
  }
  rj = calloc(1, sizeof(*rj));
  rj->journal_config = f;
  rj->jc_closure = c;
  gettimeofday(&__now, NULL);
  mtev_conf_watch_config_and_journal(NULL, EVENTER_TIMER, rj, &__now);
}

static mtev_hash_table _compiled_fallback = MTEV_HASH_EMPTY;
static struct {
  const char *key;
  const char *val;
} config_info[] = {
  /*
   * These are compile-time fallbacks to be used in the event
   * that the current running config does not have values for
   * these config paths.
   *
   * PLEASE: keep them alphabetically sorted.
   */
  { "/%s/eventer/@implementation", DEFAULT_EVENTER },
  { "/%s/modules/@directory", MTEV_MODULES_DIR },

  { NULL, NULL }
};

#define MAX_SUPPRESSIONS 128
static struct {
  xmlErrorDomain domain;
  xmlParserErrors  code;
} suppressions[MAX_SUPPRESSIONS];

static void
mtev_conf_xml_console_error_func(void *ctx, const char *format, ...) {
  mtev_console_closure_t ncct = ctx;
  va_list arg;
  if(!ncct) return;
  va_start(arg, format);
  nc_vprintf(ncct, format, arg);
  va_end(arg);
}

static void
mtev_conf_xml_console_error_ext_func(void *ctx, xmlErrorPtr err) {
  int i;
  mtev_console_closure_t ncct = ctx;
  if(!ctx) return;
  for(i=0;i<MAX_SUPPRESSIONS;i++)
    if(suppressions[i].domain == err->domain && suppressions[i].code == err->code) return;
  if(err->file)
    nc_printf(ncct, "XML error [%d/%d] in %s on line %d %s\n",
              err->domain, err->code, err->file, err->line, err->message);
  else
    nc_printf(ncct, "XML error [%d/%d] %s\n",
              err->domain, err->code, err->message);
}

static void
mtev_conf_suppress_xml_error(xmlErrorDomain domain, xmlParserErrors code) {
  int i, first_hole = -1;
  for(i=0;i<MAX_SUPPRESSIONS;i++) {
    if(suppressions[i].domain == domain && suppressions[i].code == code) return;
    if(first_hole == -1 &&
       suppressions[i].domain == XML_FROM_NONE &&
       suppressions[i].code == XML_ERR_OK)
      first_hole = i;
  }
  if(first_hole >= 0) {
    suppressions[first_hole].domain = domain;
    suppressions[first_hole].code = code;
  }
}
static void
mtev_conf_express_xml_error(xmlErrorDomain domain, xmlParserErrors code) {
  int i;
  for(i=0;i<MAX_SUPPRESSIONS;i++) {
    if(suppressions[i].domain == domain && suppressions[i].code == code) {
      suppressions[i].domain = XML_FROM_NONE;
      suppressions[i].code = XML_ERR_OK;
      return;
    }
  }
}

static void
mtev_conf_xml_error_func(void *ctx, const char *format, ...) {
  struct timeval __now;
  mtev_log_stream_t ls = ctx;
  va_list arg;
  if(!ls) return;
  va_start(arg, format);
  gettimeofday(&__now,  NULL);
  mtev_vlog(ls, &__now, __FILE__, __LINE__, format, arg);
  va_end(arg);
}
static void
mtev_conf_xml_error_ext_func(void *ctx, xmlErrorPtr err) {
  int i;
  struct timeval __now;
  mtev_log_stream_t ls = ctx;
  if(!ls) return;
  for(i=0;i<MAX_SUPPRESSIONS;i++)
    if(suppressions[i].domain == err->domain && suppressions[i].code == err->code) return;
  gettimeofday(&__now,  NULL);
  if(err->file)
    mtev_log(ls, &__now, err->file, err->line,
             "XML error [%d/%d] in %s on line %d %s\n",
             err->domain, err->code, err->file, err->line, err->message);
  else
    mtev_log(ls, &__now, err->file, err->line,
             "XML error [%d/%d] %s\n",
             err->domain, err->code, err->message);
}

void
mtev_conf_xml_errors_to_debug() {
  XML2LOG(xml_debug);
}

void
mtev_conf_poke(const char *toplevel, const char *key, const char *val) {
  char keystr[256];
  snprintf(keystr, sizeof(keystr), key, toplevel);
  mtev_hash_store(&_compiled_fallback,
                  strdup(keystr), strlen(keystr),
                  (void *)strdup(val));
}

DECLARE_CHECKER(name)
void mtev_conf_init(const char *toplevel) {
  int i;

  xml_debug = mtev_log_stream_find("debug/xml");

  COMPILE_CHECKER(name, "^[-_\\.:/a-zA-Z0-9]+$");
  XML2LOG(mtev_error);
  for(i = 0; config_info[i].key != NULL; i++)
    mtev_conf_poke(toplevel, config_info[i].key, config_info[i].val);
  xmlKeepBlanksDefault(0);
  xmlInitParser();
  xmlXPathInit();
}

void
mtev_conf_magic_separate_includes(include_node_t **root_include_nodes, int *cnt) {
  include_node_t *include_nodes = *root_include_nodes;
  assert(*cnt != -1);
  if(include_nodes) {
    int i;
    for(i=0; i<*cnt; i++) {
      mtev_conf_magic_separate_includes(&(include_nodes[i].children), &(include_nodes[i].child_count));
      if(include_nodes[i].doc) {
        xmlNodePtr n;
        for(n=include_nodes[i].insertion_point->children;
            n; n = n->next)
          n->parent = include_nodes[i].root;
        include_nodes[i].insertion_point->children =
          include_nodes[i].old_children;
        xmlFreeDoc(include_nodes[i].doc);
      }
    }
    free(include_nodes);
  }
  *root_include_nodes = NULL;
  *cnt = -1;
}

void
mtev_conf_magic_separate() {
  mtev_conf_magic_separate_includes(&config_include_nodes, &config_include_cnt);
  assert(config_include_nodes == NULL);
  if(backingstore_include_nodes) {
    int i;
    for(i=0; i<backingstore_include_cnt; i++) {
      if(backingstore_include_nodes[i].doc) {
        xmlNodePtr n;
        for(n=backingstore_include_nodes[i].insertion_point->children;
            n; n = n->next) {
          n->parent = backingstore_include_nodes[i].root;
          n->parent->last = n;
        }
        backingstore_include_nodes[i].insertion_point->children =
          backingstore_include_nodes[i].old_children;
        for(n=backingstore_include_nodes[i].insertion_point->children;
            n; n = n->next) {
          n->parent->last = n; /* sets it to the last child */
        }
        xmlFreeDoc(backingstore_include_nodes[i].doc);
      }
    }
    free(backingstore_include_nodes);
  }
  backingstore_include_nodes = NULL;
  backingstore_include_cnt = -1;
}
void
mtev_conf_kansas_city_shuffle_redo(include_node_t *include_nodes, int include_node_cnt) {
  if(include_nodes) {
    int i;
    for(i=0; i<include_node_cnt; i++) {
      mtev_conf_kansas_city_shuffle_redo(include_nodes[i].children, include_nodes[i].child_count);
      if(include_nodes[i].doc) {
        xmlNodePtr n;

        if (!include_nodes[i].snippet)
          include_nodes[i].insertion_point->children =
            include_nodes[i].root->children;
        else
          include_nodes[i].insertion_point->children =
            include_nodes[i].root;

        for(n=include_nodes[i].insertion_point->children;
            n; n = n->next)
          n->parent = include_nodes[i].insertion_point;
      }
    }
  }
}
void
mtev_conf_kansas_city_shuffle_undo(include_node_t *include_nodes, int include_node_cnt) {
  if(include_nodes) {
    int i;
    for(i=0; i<include_node_cnt; i++) {
      mtev_conf_kansas_city_shuffle_undo(include_nodes[i].children, include_nodes[i].child_count);
      if(include_nodes[i].doc) {
        xmlNodePtr n;
        for(n=include_nodes[i].insertion_point->children;
            n; n = n->next)
          n->parent = include_nodes[i].root;
        include_nodes[i].insertion_point->children =
          include_nodes[i].old_children;
      }
    }
  }
}
static u_int64_t
usec_now() {
  u_int64_t usec;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  usec = tv.tv_sec * 1000000UL;
  usec += tv.tv_usec;
  return usec;
}

static void
remove_emancipated_child_node(xmlNodePtr oldp, xmlNodePtr node) {
  /* node was once a child of oldp... it's still in it's children list
   * but node's parent isn't this child.
   */
  assert(node->parent != oldp);
  if(oldp->children == NULL) return;
  if(oldp->children == node) {
    oldp->children = node->next;
    if (node->next) node->next->prev = node->prev;
  }
  else {
    xmlNodePtr prev = oldp->children;
    for(prev = oldp->children; prev->next && prev->next != node; prev = prev->next);
    if(prev) prev->next = node->next;
    if(node->next) node->next->prev = prev;
  }
}
void
mtev_conf_include_remove(mtev_conf_section_t vnode) {
  int i;
  xmlNodePtr node = vnode;
  for(i=0;i<config_include_cnt;i++) {
    if(node->parent == config_include_nodes[i].insertion_point) {
      remove_emancipated_child_node(config_include_nodes[i].root, node);
    }
  }
}
void
mtev_conf_backingstore_remove(mtev_conf_section_t vnode) {
  int i;
  xmlNodePtr node = vnode;
  mtev_xml_userdata_t *subctx = node->_private;
  for(i=0;i<backingstore_include_cnt;i++) {
    if(node->parent == backingstore_include_nodes[i].insertion_point) {
      remove_emancipated_child_node(backingstore_include_nodes[i].root, node);
    }
  }
  if(subctx) {
    mtevL(mtev_debug, "marking %s for removal\n", subctx->path);
    if(!backingstore_freelist) backingstore_freelist = subctx;
    else {
      mtev_xml_userdata_t *fl = backingstore_freelist;
      while(fl->freelist) fl = fl->freelist;
      fl->freelist = subctx;
    }
    node->_private = NULL;
  }
  /* If we're deleted, we'll mark the parent as dirty */
  if(node->parent) mtev_conf_backingstore_dirty(node->parent);
}
void
mtev_conf_backingstore_dirty(mtev_conf_section_t vnode) {
  xmlNodePtr node = vnode;
  mtev_xml_userdata_t *subctx = node->_private;
  if(subctx) {
    mtevL(mtev_debug, "backingstore[%s] marked dirty\n", subctx->path);
    subctx->dirty_time = usec_now();
    return;
  }
  if(node->parent) mtev_conf_backingstore_dirty(node->parent);
}
int
mtev_conf_backingstore_write(mtev_xml_userdata_t *ctx, mtev_boolean skip,
                             xmlAttrPtr attrs, xmlNodePtr node) {
  int failure = 0;
  char newpath[PATH_MAX];
  xmlNodePtr n;
  snprintf(newpath, sizeof(newpath), "%s/.attrs", ctx->path);
  if(attrs) {
    xmlDocPtr tmpdoc;
    xmlNodePtr tmpnode;
    mtevL(mtev_debug, " **> %s\n", newpath);
    tmpdoc = xmlNewDoc((xmlChar *)"1.0");
    tmpnode = xmlNewNode(NULL, ctx->name ? (xmlChar *)ctx->name : (xmlChar *)"stub");
    xmlDocSetRootElement(tmpdoc, tmpnode);
    tmpnode->properties = attrs;
    failure = mtev_xmlSaveToFile(tmpdoc, newpath);
    tmpnode->properties = NULL;
    xmlFreeDoc(tmpdoc);
    if(failure) return -1;
  }
  else if(!skip) {
    unlink(newpath);
  }
  for(n = node; n; n = n->next) {
    int leaf;
    mtev_xml_userdata_t *subctx;
    subctx = n->_private;
    leaf = is_stopnode(n);
    if(!subctx) { /* This has never been written out */
      subctx = calloc(1, sizeof(*subctx));
      subctx->name = strdup((char *)n->name);
      snprintf(newpath, sizeof(newpath), "%s/%s#%llu", ctx->path, n->name,
               (unsigned long long)++max_gen_count);
      if(leaf) strlcat(newpath, ".xml", sizeof(newpath));
      subctx->path = strdup(newpath);
      subctx->dirty_time = usec_now();
      n->_private = subctx;
      mtevL(mtev_debug, " !!> %s\n", subctx->path);
    }
    if(leaf) {
      xmlDocPtr tmpdoc;
      xmlNodePtr tmpnode;
      if(subctx->dirty_time > last_config_flush) {
        xmlNsPtr *parent_nslist, iter_ns;
        xmlNodePtr root;
        root = xmlDocGetRootElement(master_config);
        parent_nslist = xmlGetNsList(master_config, root);

        tmpdoc = xmlNewDoc((xmlChar *)"1.0");
        tmpnode = xmlNewNode(NULL, n->name);
        xmlDocSetRootElement(tmpdoc, tmpnode);
        if(parent_nslist) {
          for(iter_ns = *parent_nslist; iter_ns; iter_ns = iter_ns->next)
            xmlNewNs(tmpnode, iter_ns->href, iter_ns->prefix);
          xmlFree(parent_nslist);
        }
        tmpnode->properties = n->properties;
        tmpnode->children = n->children;
        failure = mtev_xmlSaveToFile(tmpdoc, subctx->path);
        tmpnode->properties = NULL;
        tmpnode->children = NULL;
        xmlFreeDoc(tmpdoc);
        mtevL(mtev_debug, " ==> %s\n", subctx->path);
        if(failure) return -1;
      }
    }
    else {
      mtev_boolean skip_attrs;
      skip_attrs = leaf || (subctx->dirty_time <= last_config_flush);
      mtevL(mtev_debug, " --> %s\n", subctx->path);
      if(mtev_conf_backingstore_write(subctx, skip_attrs, skip_attrs ? NULL : n->properties, n->children))
        return -1;
    }
  }
  return 0;
}
void
mtev_conf_shatter_write(xmlDocPtr doc) {
  if(backingstore_freelist) {
    mtev_xml_userdata_t *fl, *last;
    for(fl = backingstore_freelist; fl; ) {
      last = fl;
      fl = fl->freelist;
      /* If it is a file, we'll unlink it, otherwise,
       * we need to delete the attributes and the directory.
       */
      if(unlink(last->path)) {
        char attrpath[PATH_MAX];
        snprintf(attrpath, sizeof(attrpath), "%s/.attrs", last->path);
        unlink(attrpath);
        if(rmdir(last->path) && errno != ENOENT) {
          /* This shouldn't happen, but if it does we risk
           * leaving a mess. Don't do that.
           */
          mtevL(mtev_error, "backingstore mess %s: %s\n",
                last->path, strerror(errno));
        }
      }
      mtev_xml_userdata_free(last);
    }
    backingstore_freelist = NULL;
  }
  if(backingstore_include_nodes) {
    int i;
    for(i=0; i<backingstore_include_cnt; i++) {
      if(backingstore_include_nodes[i].doc) {
        xmlNodePtr n;
        mtev_xml_userdata_t *what = backingstore_include_nodes[i].doc->_private;

        for(n=backingstore_include_nodes[i].insertion_point->children;
            n; n = n->next) {
          n->parent = backingstore_include_nodes[i].root;
          n->parent->last = n;
        }
        backingstore_include_nodes[i].root->children =
          backingstore_include_nodes[i].insertion_point->children;
        backingstore_include_nodes[i].insertion_point->children =
          backingstore_include_nodes[i].old_children;
        for(n=backingstore_include_nodes[i].insertion_point->children;
            n; n = n->next) {
          n->parent->last = n; /* sets it to the last child */
        }
        mtev_conf_backingstore_write(what, mtev_false, NULL, backingstore_include_nodes[i].root->children);
      }
    }
    last_config_flush = usec_now();
  }
}
void
mtev_conf_shatter_postwrite(xmlDocPtr doc) {
  if(backingstore_include_nodes) {
    int i;
    for(i=0; i<backingstore_include_cnt; i++) {
      if(backingstore_include_nodes[i].doc) {
        xmlNodePtr n;
        backingstore_include_nodes[i].insertion_point->children =
          backingstore_include_nodes[i].root->children;
        for(n=backingstore_include_nodes[i].insertion_point->children;
            n; n = n->next) {
          n->parent = backingstore_include_nodes[i].insertion_point;
          n->parent->last = n;
        }
      }
    }
  }
}

int
mtev_conf_read_into_node(xmlNodePtr node, const char *path) {
  DIR *dirroot;
  struct dirent *de, *entry;
  char filepath[PATH_MAX];
  xmlDocPtr doc;
  xmlNodePtr root = NULL;
  struct stat sb;
  int size, rv;

  mtevL(mtev_debug, "read backing store: %s\n", path);
  snprintf(filepath, sizeof(filepath), "%s/.attrs", path);
  while((rv = stat(filepath, &sb)) < 0 && errno == EINTR);
  if(rv == 0) {
    doc = xmlReadFile(filepath, "utf8", XML_PARSE_NOENT);
    if(doc) root = xmlDocGetRootElement(doc);
    if(doc && root) {
      node->properties = xmlCopyPropList(node, root->properties);
      xmlFreeDoc(doc);
      doc = NULL;
    }
  }
#ifdef _PC_NAME_MAX
  size = pathconf(path, _PC_NAME_MAX);
#endif
  size = MAX(size, PATH_MAX + 128);
  de = alloca(size);
  dirroot = opendir(path);
  if(!dirroot) return -1;
  while(portable_readdir_r(dirroot, de, &entry) == 0 && entry != NULL) {
    mtev_xml_userdata_t *udata;
    char name[PATH_MAX];
    char *sep;
    xmlNodePtr child;
    u_int64_t gen;

    mtev_watchdog_child_heartbeat();

    sep = strchr(entry->d_name, '#');
    if(!sep) continue;
    snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
    while((rv = stat(filepath, &sb)) < 0 && errno == EINTR);
    if(rv == 0) {
      strlcpy(name, entry->d_name, sizeof(name));
      name[sep - entry->d_name] = '\0';
      gen = strtoull(sep+1, NULL, 10);
      if(gen > max_gen_count) max_gen_count = gen;

      if(S_ISDIR(sb.st_mode)) {
        mtevL(mtev_debug, "<DIR< %s\n", entry->d_name);
        child = xmlNewNode(NULL, (xmlChar *)name);
        mtev_conf_read_into_node(child, filepath);
        udata = calloc(1, sizeof(*udata));
        udata->name = strdup(name);
        udata->path = strdup(filepath);
        child->_private = udata;
        xmlAddChild(node, child);
      }
      else if(S_ISREG(sb.st_mode)) {
        xmlDocPtr cdoc;
        xmlNodePtr cnode = NULL;
        mtevL(mtev_debug, "<FILE< %s\n", entry->d_name);
        cdoc = xmlParseFile(filepath);
        if(cdoc) {
          cnode = xmlDocGetRootElement(cdoc);
          xmlDocSetRootElement(cdoc, xmlNewNode(NULL, (xmlChar *)"dummy"));
          if(cnode) {
            udata = calloc(1, sizeof(*udata));
            udata->name = strdup(name);
            udata->path = strdup(filepath);
            cnode->_private = udata;
            xmlAddChild(node, cnode);
          }
          xmlFreeDoc(cdoc);
        }
      }
    }
  }
  closedir(dirroot);
  return 0;
}

xmlDocPtr
mtev_conf_read_backing_store(const char *path) {
  xmlDocPtr doc;
  xmlNodePtr root;
  mtev_xml_userdata_t *what;

  doc = xmlNewDoc((xmlChar *)"1.0");
  what = calloc(1, sizeof(*what));
  what->path = strdup(path);
  doc->_private = what;
  root = xmlNewNode(NULL, (xmlChar *)"stub");
  xmlDocSetRootElement(doc, root);
  mtev_conf_read_into_node(root, path);
  return doc;
}
int
mtev_conf_magic_mix(const char *parentfile, xmlDocPtr doc, include_node_t* inc_node) {
  xmlXPathContextPtr mix_ctxt = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlNodePtr node;
  int i, cnt, rv = 0, master = 0;
  int *include_cnt;
  include_node_t* include_nodes;

  if (inc_node) {
    include_cnt = &(inc_node->child_count);
    include_nodes = inc_node->children;
  }
  else {
    include_cnt = &(config_include_cnt);
    include_nodes = config_include_nodes;
    master = 1;
  }

  assert(*include_cnt == -1);

  if (master) {
    assert(backingstore_include_cnt == -1);
    backingstore_include_cnt = 0;
  }
  mix_ctxt = xmlXPathNewContext(doc);
  if (master)
    pobj = xmlXPathEval((xmlChar *)"//*[@backingstore]", mix_ctxt);
  else
    pobj = NULL;

  if(!pobj) goto includes;
  if(pobj->type != XPATH_NODESET) goto includes;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto includes;

  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(cnt > 0)
    backingstore_include_nodes = calloc(cnt, sizeof(*backingstore_include_nodes));
  for(i=0; i<cnt; i++) {
    char *path, *infile;
    node = xmlXPathNodeSetItem(pobj->nodesetval, i);
    path = (char *)xmlGetProp(node, (xmlChar *)"backingstore");
    if(!path) continue;
    if(*path == '/') infile = strdup(path);
    else {
      char *cp;
      infile = malloc(PATH_MAX);
      strlcpy(infile, parentfile, PATH_MAX);
      for(cp = infile + strlen(infile) - 1; cp > infile; cp--) {
        if(*cp == '/') { *cp = '\0'; break; }
        else *cp = '\0';
      }
      strlcat(infile, "/", PATH_MAX);
      strlcat(infile, path, PATH_MAX);
    }
    xmlFree(path);
    backingstore_include_nodes[i].doc = mtev_conf_read_backing_store(infile);
    if(backingstore_include_nodes[i].doc) {
      xmlNodePtr n, lchild;
      backingstore_include_nodes[i].insertion_point = node;
      backingstore_include_nodes[i].root = xmlDocGetRootElement(backingstore_include_nodes[i].doc);
      /* for backing store, they are permanently reattached under the backing store.
       * so for any children, we need to glue them into the new parent document.
       */
      lchild = backingstore_include_nodes[i].root->children;
      while(lchild && lchild->next) lchild = lchild->next;
      if(lchild) {
        lchild->next = node->children;
        if(node->children) node->children->prev = lchild;
      }
      else
        backingstore_include_nodes[i].root->children = node->children;
      for(n=node->children; n; n = n->next) {
        n->parent = backingstore_include_nodes[i].root; /* this gets mapped right back, just for clarity */
        n->doc = backingstore_include_nodes[i].doc;
      }
      backingstore_include_nodes[i].old_children = NULL;
      node->children = backingstore_include_nodes[i].root->children;
      for(n=node->children; n; n = n->next) {
        n->parent = backingstore_include_nodes[i].insertion_point;
        n->parent->last = n;
      }
    }
    else {
      mtevL(mtev_error, "Could not load: '%s'\n", infile);
      rv = -1;
    }
    free(infile);
  }
  if(mix_ctxt) xmlXPathFreeContext(mix_ctxt);
  mix_ctxt = xmlXPathNewContext(doc);
  backingstore_include_cnt = cnt;
  mtevL(mtev_debug, "Processed %d backing stores.\n", backingstore_include_cnt);

 includes:
  if(pobj) xmlXPathFreeObject(pobj);
  *include_cnt = 0;
  pobj = xmlXPathEval((xmlChar *)"//include[@file]", mix_ctxt);
  if(!pobj) goto out;
  if(pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  if(cnt > 0) {
    include_nodes = calloc(cnt, sizeof(*include_nodes));
    if (master) {
      config_include_nodes = include_nodes;
    }
    else {
      inc_node->children = include_nodes;
    }
    for (i=0; i < cnt; i++) {
      include_nodes[i].child_count = -1;
    }
  }
  for(i=0; i<cnt; i++) {
    char *path, *infile, *snippet, *ro;
    node = xmlXPathNodeSetItem(pobj->nodesetval, i);
    path = (char *)xmlGetProp(node, (xmlChar *)"file");
    if(!path) continue;
    snippet = (char *)xmlGetProp(node, (xmlChar *)"snippet");
    include_nodes[i].snippet = (snippet && strcmp(snippet, "false"));
    if(snippet) xmlFree(snippet);
    if(*path == '/') infile = strdup(path);
    else {
      char *cp;
      infile = malloc(PATH_MAX);
      strlcpy(infile, parentfile, PATH_MAX);
      for(cp = infile + strlen(infile) - 1; cp > infile; cp--) {
        if(*cp == '/') { *cp = '\0'; break; }
        else *cp = '\0';
      }
      strlcat(infile, "/", PATH_MAX);
      strlcat(infile, path, PATH_MAX);
    }
    xmlFree(path);
    ro = (char *)xmlGetProp(node, (xmlChar *)"readonly");
    if (ro && !strcmp(ro, "true")) include_nodes[i].ro = 1;
    if (ro) xmlFree(ro);
    if (include_nodes[i].snippet) {
      mtev_conf_suppress_xml_error(XML_FROM_IO, XML_IO_LOAD_ERROR);
      include_nodes[i].doc = xmlParseEntity(infile);
      mtev_conf_express_xml_error(XML_FROM_IO, XML_IO_LOAD_ERROR);
    }
    else
      include_nodes[i].doc = xmlReadFile(infile, "utf8", XML_PARSE_NOENT);
    if((include_nodes[i].doc) || (include_nodes[i].snippet)) {
      xmlNodePtr n;
      mtev_conf_magic_mix(infile, include_nodes[i].doc, &(include_nodes[i]));
      strncpy(include_nodes[i].path, infile, sizeof(include_nodes[i].path));
      include_nodes[i].insertion_point = node;
      include_nodes[i].root = xmlDocGetRootElement(include_nodes[i].doc);
      include_nodes[i].old_children = node->children;
      if (!include_nodes[i].snippet)
        node->children = include_nodes[i].root->children;
      else
        node->children = include_nodes[i].root;
      for(n=node->children; n; n = n->next)
        n->parent = include_nodes[i].insertion_point;
    }
    else {
      mtevL(mtev_error, "Could not load: '%s'\n", infile);
      rv = -1;
    }
    free(infile);
  }
  *include_cnt = cnt;
  mtevL(mtev_debug, "Processed %d includes\n", *include_cnt);
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  if(mix_ctxt) xmlXPathFreeContext(mix_ctxt);
  return rv;
}

static int mtev_conf_load_internal(const char *path) {
  int rv = 0;
  xmlDocPtr new_config;
  xmlNodePtr root;
  new_config = xmlParseFile(path);
  if(new_config) {
    root = xmlDocGetRootElement(new_config);
    if(root_node_name) free(root_node_name);
    root_node_name = strdup((char *)root->name);

    if(master_config) {
      /* separate all includes */
      mtev_conf_magic_separate();
      xmlFreeDoc(master_config);
    }
    if(xpath_ctxt) xmlXPathFreeContext(xpath_ctxt);

    master_config = new_config;
    /* mixin all the includes */
    if(mtev_conf_magic_mix(path, master_config, NULL)) rv = -1;

    xpath_ctxt = xmlXPathNewContext(master_config);
    if(path != master_config_file)
      if(realpath(path, master_config_file) == NULL)
        mtevL(mtev_error, "realpath failed: %s\n", strerror(errno));
    mtev_conf_mark_changed();
    return rv;
  }
  rv = -1;
  return rv;
}
int mtev_conf_load(const char *path) {
  int rv;
  XML2LOG(mtev_error);
  rv = mtev_conf_load_internal(path);
  XML2LOG(xml_debug);
  return rv;
}

char *mtev_conf_config_filename() {
  return strdup(master_config_file);
}

int mtev_conf_xml_xpath(xmlDocPtr *mc, xmlXPathContextPtr *xp) {
  if(mc) *mc = master_config;
  if(xp) *xp = xpath_ctxt;
  return 0;
}
int mtev_conf_save(const char *path) {
  return -1;
}

void mtev_conf_get_elements_into_hash(mtev_conf_section_t section,
                                      const char *path,
                                      mtev_hash_table *table,
                                      const char *namespace) {
  int i, cnt;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr current_ctxt;
  xmlNodePtr current_node = (xmlNodePtr)section;
  xmlNodePtr node;

  current_ctxt = xpath_ctxt;
  if(current_node) {
    current_ctxt = xmlXPathNewContext(master_config);
    current_ctxt->node = current_node;
  }
  pobj = xmlXPathEval((xmlChar *)path, current_ctxt);
  if(!pobj) goto out;
  if(pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  for(i=0; i<cnt; i++) {
    const xmlChar *name;
    int freename = 0;
    char *value;
    node = xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(namespace && node->ns && !strcmp((char *)node->ns->prefix, namespace)) {
      name = node->name;
      if(!strcmp((char *)name, "value")) {
        name = xmlGetProp(node, (xmlChar *)"name");
        if(!name) name = node->name;
        else freename = 1;
      }
      value = (char *)xmlXPathCastNodeToString(node);
      mtev_hash_replace(table,
                        strdup((char *)name), strlen((char *)name),
                        strdup(value), free, free);
      xmlFree(value);
    }
    else if(!namespace && !node->ns) {
      name = node->name;
      if(!strcmp((char *)name, "value")) {
        name = xmlGetProp(node, (xmlChar *)"name");
        if(!name) name = node->name;
        else freename = 1;
      }
      value = (char *)xmlXPathCastNodeToString(node);
      mtev_hash_replace(table,
                        strdup((char *)name), strlen((char *)name),
                        strdup(value), free, free);
      xmlFree(value);
    }
    if(freename) xmlFree((void *)name);
  }
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  if(current_ctxt && current_ctxt != xpath_ctxt)
    xmlXPathFreeContext(current_ctxt);
}
void mtev_conf_get_into_hash(mtev_conf_section_t section,
                             const char *path,
                             mtev_hash_table *table,
                             const char *namespace) {
  int cnt;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr current_ctxt;
  xmlNodePtr current_node = (xmlNodePtr)section;
  xmlNodePtr node, parent_node;
  char xpath_expr[1024];
  char *inheritid;

  current_ctxt = xpath_ctxt;
  if(current_node) {
    current_ctxt = xmlXPathNewContext(master_config);
    current_ctxt->node = current_node;
  }
  if(path[0] == '/')
    strlcpy(xpath_expr, path, sizeof(xpath_expr));
  else
    snprintf(xpath_expr, sizeof(xpath_expr),
             "ancestor-or-self::node()/%s", path);
  pobj = xmlXPathEval((xmlChar *)xpath_expr, current_ctxt);
  if(!pobj) goto out;
  if(pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto out;
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  /* These are in the order of root to leaf
   * We want to recurse... apply:
   *   1. our parent's config
   *   2. our "inherit" config if it exists.
   *   3. our config.
   */
  node = xmlXPathNodeSetItem(pobj->nodesetval, cnt-1);
  /* 1. */
  if(cnt > 1 && node) {
    parent_node = xmlXPathNodeSetItem(pobj->nodesetval, cnt-2);
    if(parent_node != current_node)
      mtev_conf_get_into_hash(parent_node, (const char *)node->name, table, namespace);
  }
  /* 2. */
  inheritid = (char *)xmlGetProp(node, (xmlChar *)"inherit");
  if(inheritid) {
    snprintf(xpath_expr, sizeof(xpath_expr), "//*[@id=\"%s\"]", inheritid);
    mtev_conf_get_into_hash(NULL, xpath_expr, table, namespace);
    xmlFree(inheritid);
  }
  /* 3. */
  mtev_conf_get_elements_into_hash(node, "*", table, namespace);

 out:
  if(pobj) xmlXPathFreeObject(pobj);
  if(current_ctxt && current_ctxt != xpath_ctxt)
    xmlXPathFreeContext(current_ctxt);
}
mtev_hash_table *mtev_conf_get_namespaced_hash(mtev_conf_section_t section,
                                               const char *path, const char *ns) {
  mtev_hash_table *table = NULL;

  table = calloc(1, sizeof(*table));
  mtev_conf_get_into_hash(section, path, table, ns);
  if(mtev_hash_size(table) == 0) {
    mtev_hash_destroy(table, free, free);
    free(table);
    table = NULL;
  }
  return table;
}
mtev_hash_table *mtev_conf_get_hash(mtev_conf_section_t section,
                                    const char *path) {
  mtev_hash_table *table = NULL;

  table = calloc(1, sizeof(*table));
  mtev_conf_get_into_hash(section, path, table, NULL);
  return table;
}
mtev_conf_section_t mtev_conf_get_section(mtev_conf_section_t section,
                                          const char *path) {
  mtev_conf_section_t subsection = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr current_ctxt;
  xmlNodePtr current_node = (xmlNodePtr)section;

  current_ctxt = xpath_ctxt;
  if(current_node) {
    current_ctxt = xmlXPathNewContext(master_config);
    current_ctxt->node = current_node;
  }
  pobj = xmlXPathEval((xmlChar *)path, current_ctxt);
  if(!pobj) goto out;
  if(pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto out;
  subsection = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  if(current_ctxt && current_ctxt != xpath_ctxt)
    xmlXPathFreeContext(current_ctxt);
  return subsection;
}
mtev_conf_section_t *mtev_conf_get_sections(mtev_conf_section_t section,
                                            const char *path,
                                            int *cnt) {
  int i;
  mtev_conf_section_t *sections = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr current_ctxt;
  xmlNodePtr current_node = (xmlNodePtr)section;

  *cnt = 0;
  current_ctxt = xpath_ctxt;
  if(current_node) {
    current_ctxt = xmlXPathNewContext(master_config);
    current_ctxt->node = current_node;
  }
  pobj = xmlXPathEval((xmlChar *)path, current_ctxt);
  if(!pobj) goto out;
  if(pobj->type != XPATH_NODESET) goto out;
  if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto out;
  *cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  sections = calloc(*cnt, sizeof(*sections));
  for(i=0; i<*cnt; i++)
    sections[i] = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
 out:
  if(pobj) xmlXPathFreeObject(pobj);
  if(current_ctxt && current_ctxt != xpath_ctxt)
    xmlXPathFreeContext(current_ctxt);
  return sections;
}
int mtev_conf_remove_section(mtev_conf_section_t section) {
  if (!section) return -1;
  xmlNodePtr node = (xmlNodePtr) section;
  xmlUnlinkNode(node);
  xmlFreeNode(node);
  mtev_conf_mark_changed();
  return 0;
}
int _mtev_conf_get_string(mtev_conf_section_t section, xmlNodePtr *vnode,
                          const char *path, char **value) {
  const char *str, *interest;
  char fullpath[1024];
  int rv = 1, i;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr current_ctxt;
  xmlNodePtr current_node = (xmlNodePtr)section;

  current_ctxt = xpath_ctxt;
  if(current_node) {
    current_ctxt = xmlXPathNewContext(master_config);
    current_ctxt->node = current_node;
  }
  pobj = xmlXPathEval((xmlChar *)path, current_ctxt);
  if(pobj) {
    xmlNodePtr node;
    switch(pobj->type) {
      case XPATH_NODESET:
        if(xmlXPathNodeSetIsEmpty(pobj->nodesetval)) goto fallback;
        i = xmlXPathNodeSetGetLength(pobj->nodesetval);
        node = xmlXPathNodeSetItem(pobj->nodesetval, i-1);
        if(vnode) *vnode = node;
        *value = (char *)xmlXPathCastNodeToString(node);
        break;
      default:
        *value = (char *)xmlXPathCastToString(pobj);
    }
    goto found;
  }
 fallback:
  interest = path;
  if(*interest != '/' && current_node) {
    xmlChar *basepath = xmlGetNodePath(current_node);
    snprintf(fullpath, sizeof(fullpath), "%s/%s", (char *)basepath, path);
    free(basepath);
    interest = fullpath;
  }
  if(mtev_hash_retr_str(&_compiled_fallback,
                        interest, strlen(interest), &str)) {
    *value = (char *)xmlStrdup((xmlChar *)str);
    goto found;
  }
  rv = 0;
 found:
  if(pobj) xmlXPathFreeObject(pobj);
  if(current_ctxt && current_ctxt != xpath_ctxt)
    xmlXPathFreeContext(current_ctxt);
  return rv;
}
int mtev_conf_get_uuid(mtev_conf_section_t section,
                       const char *path, uuid_t out) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    if(uuid_parse(str, out) == 0) return 1;
    return 0;
  }
  return 0;
}
int mtev_conf_get_string(mtev_conf_section_t section,
                         const char *path, char **value) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    *value = strdup(str);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_get_stringbuf(mtev_conf_section_t section,
                            const char *path, char *buf, int len) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    strlcpy(buf, str, len);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_set_string(mtev_conf_section_t section,
                         const char *path, const char *value) {
  xmlNodePtr current_node = (xmlNodePtr)section;
  if(strchr(path, '/')) return 0;
  if(path[0] == '@') {
    xmlSetProp(current_node, (xmlChar *)path+1, (xmlChar *)value);
    CONF_DIRTY(current_node);
  }
  else {
    xmlNodePtr child_node;
    if(value) {
      child_node = xmlNewTextChild(current_node, NULL, (xmlChar *)path, (xmlChar *)value);
    }
    else {
      child_node = xmlNewChild(current_node, NULL, (xmlChar *)path, NULL);
    }
    CONF_DIRTY(child_node);
  }
  mtev_conf_mark_changed();
  if(mtev_conf_write_file(NULL) != 0)
    mtevL(mtev_error, "local config write failed\n");
  return 1;
}
int mtev_conf_string_to_int(const char *str) {
  int base = 10;
  if(!str) return 0;
  if(str[0] == '0') {
    if(str[1] == 'x') base = 16;
    else base = 8;
  }
  return strtol(str, NULL, base);
}
int mtev_conf_get_int(mtev_conf_section_t section,
                      const char *path, int *value) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    *value = (int)mtev_conf_string_to_int(str);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_get_int64(mtev_conf_section_t section,
                        const char *path, int64_t *value) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    *value = strtoll(str, NULL, 10);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_set_int(mtev_conf_section_t section,
                      const char *path, int value) {
  char buffer[32];
  snprintf(buffer, 32, "%d", value);
  return mtev_conf_set_string(section,path,buffer);
}
float mtev_conf_string_to_float(const char *str) {
  if(!str) return 0.0;
  return atof(str);
}
int mtev_conf_get_float(mtev_conf_section_t section,
                        const char *path, float *value) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    *value = mtev_conf_string_to_float(str);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_set_float(mtev_conf_section_t section,
                        const char *path, float value) {
  char buffer[32];
  snprintf(buffer, 32, "%f", value);
  return mtev_conf_set_string(section,path,buffer);
}
mtev_boolean mtev_conf_string_to_boolean(const char *str) {
  if(!str) return mtev_false;
  if(!strcasecmp(str, "true") || !strcasecmp(str, "on")) return mtev_true;
  return mtev_false;
}
int mtev_conf_get_boolean(mtev_conf_section_t section,
                          const char *path, mtev_boolean *value) {
  char *str;
  if(_mtev_conf_get_string(section,NULL,path,&str)) {
    *value = mtev_conf_string_to_boolean(str);
    xmlFree(str);
    return 1;
  }
  return 0;
}
int mtev_conf_set_boolean(mtev_conf_section_t section,
                          const char *path, mtev_boolean value) {
  if(value == mtev_true)
    return mtev_conf_set_string(section,path,"true");
  return mtev_conf_set_string(section,path,"false");
}

int mtev_conf_should_resolve_targets(mtev_boolean *should_resolve) {
  static int inited = 0, cached_rv;;
  static mtev_boolean cached_should_resolve;
  if(!inited) {
    cached_rv = mtev_conf_get_boolean(NULL, "//checks/@resolve_targets",
                                      &cached_should_resolve);
    inited = 1;
  }
  *should_resolve = cached_should_resolve;
  return cached_rv;
}

struct config_line_vstr {
  char *buff;
  int raw_len;
  int len;
  int allocd;
  enum { CONFIG_RAW = 0, CONFIG_COMPRESSED, CONFIG_B64 } target, encoded;
};
static int
mtev_config_log_write_xml(void *vstr, const char *buffer, int len) {
  struct config_line_vstr *clv = vstr;
  assert(clv->encoded == CONFIG_RAW);
  if(!clv->buff) {
    clv->allocd = 8192;
    clv->buff = malloc(clv->allocd);
  }
  while(len + clv->len > clv->allocd) {
    char *newbuff;
    int newsize = clv->allocd;
    newsize <<= 1;
    newbuff = realloc(clv->buff, newsize);
    if(!newbuff) {
      return -1;
    }
    clv->allocd = newsize;
    clv->buff = newbuff;
  }
  memcpy(clv->buff + clv->len, buffer, len);
  clv->len += len;
  return len;
}
static int
mtev_config_log_close_xml(void *vstr) {
  struct config_line_vstr *clv = vstr;
  uLong initial_dlen, dlen;
  char *compbuff, *b64buff;

  if(clv->buff == NULL) {
    clv->encoded = clv->target;
    return 0;
  }
  clv->raw_len = clv->len;
  assert(clv->encoded == CONFIG_RAW);
  if(clv->encoded == clv->target) return 0;

  /* Compress */
  initial_dlen = dlen = compressBound(clv->len);
  compbuff = malloc(initial_dlen);
  if(!compbuff) return -1;
  if(Z_OK != compress2((Bytef *)compbuff, &dlen,
                       (Bytef *)clv->buff, clv->len, 9)) {
    mtevL(mtev_error, "Error compressing config for transmission.\n");
    free(compbuff);
    return -1;
  }
  free(clv->buff);
  clv->buff = compbuff;
  clv->allocd = initial_dlen;
  clv->len = dlen;
  clv->encoded = CONFIG_COMPRESSED;
  if(clv->encoded == clv->target) return 0;

  /* Encode */
  initial_dlen = ((clv->len + 2) / 3) * 4;
  b64buff = malloc(initial_dlen);
  dlen = mtev_b64_encode((unsigned char *)clv->buff, clv->len,
                         b64buff, initial_dlen);
  if(dlen == 0) {
    free(b64buff);
    return -1;
  }
  free(clv->buff);
  clv->buff = b64buff;
  clv->allocd = initial_dlen;
  clv->len = dlen;
  clv->encoded = CONFIG_B64;
  if(clv->encoded == clv->target) return 0;
  return -1;
}

int
mtev_conf_reload(mtev_console_closure_t ncct,
                 int argc, char **argv,
                 mtev_console_state_t *state, void *closure) {
  XML2CONSOLE(ncct);
  if(mtev_conf_load_internal(master_config_file)) {
    XML2LOG(xml_debug);
    nc_printf(ncct, "error loading config\n");
    return -1;
  }
  XML2LOG(xml_debug);
  return 0;
}
int
mtev_conf_write_terminal(mtev_console_closure_t ncct,
                         int argc, char **argv,
                         mtev_console_state_t *state, void *closure) {
  xmlOutputBufferPtr out;
  xmlCharEncodingHandlerPtr enc;
  enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
  out = xmlOutputBufferCreateIO(mtev_console_write_xml,
                                mtev_console_close_xml,
                                ncct, enc);
  mtev_conf_kansas_city_shuffle_undo(config_include_nodes, config_include_cnt);
  xmlSaveFormatFileTo(out, master_config, "utf8", 1);
  write_out_include_files(config_include_nodes, config_include_cnt);
  mtev_conf_kansas_city_shuffle_redo(config_include_nodes, config_include_cnt);
  return 0;
}
int
mtev_conf_write_file_console(mtev_console_closure_t ncct,
                             int argc, char **argv,
                             mtev_console_state_t *state, void *closure) {
  int rv;
  char *err = NULL;
  rv = mtev_conf_write_file(&err);
  nc_printf(ncct, "%s\n", err);
  if(err) free(err);
  return rv;
}
int
mtev_conf_write_file(char **err) {
  int fd, len;
  char master_file_tmp[PATH_MAX];
  char errstr[1024];
  xmlOutputBufferPtr out;
  xmlCharEncodingHandlerPtr enc;
  struct stat st;
  mode_t mode = 0640; /* the default */
  uid_t uid = 0;
  gid_t gid = 0;

  if(stat(master_config_file, &st) == 0) {
    mode = st.st_mode;
    uid = st.st_uid;
    gid = st.st_gid;
  }
  snprintf(master_file_tmp, sizeof(master_file_tmp),
           "%s.tmp", master_config_file);
  unlink(master_file_tmp);
  fd = open(master_file_tmp, O_CREAT|O_EXCL|O_WRONLY|NE_O_CLOEXEC, mode);
  if(fd < 0) {
    snprintf(errstr, sizeof(errstr), "Failed to open tmp file: %s",
             strerror(errno));
    if(err) *err = strdup(errstr);
    return -1;
  }
  fchown(fd, uid, gid);

  enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
  out = xmlOutputBufferCreateFd(fd, enc);
  if(!out) {
    close(fd);
    unlink(master_file_tmp);
    if(err) *err = strdup("internal error: OutputBufferCreate failed");
    return -1;
  }
  mtev_conf_kansas_city_shuffle_undo(config_include_nodes, config_include_cnt);
  mtev_conf_shatter_write(master_config);
  len = xmlSaveFormatFileTo(out, master_config, "utf8", 1);
  mtev_conf_shatter_postwrite(master_config);
  write_out_include_files(config_include_nodes, config_include_cnt);
  mtev_conf_kansas_city_shuffle_redo(config_include_nodes, config_include_cnt);
  close(fd);
  if(len <= 0) {
    if(err) *err = strdup("internal error: writing to tmp file failed.");
    return -1;
  }
  if(rename(master_file_tmp, master_config_file) != 0) {
    snprintf(errstr, sizeof(errstr), "Failed to replace file: %s",
             strerror(errno));
    if(err) *err = strdup(errstr);
    return -1;
  }
  snprintf(errstr, sizeof(errstr), "%d bytes written.", len);
  if(err) *err = strdup(errstr);
  return 0;
}
char *
mtev_conf_xml_in_mem(size_t *len) {
  struct config_line_vstr *clv;
  xmlOutputBufferPtr out;
  xmlCharEncodingHandlerPtr enc;
  char *rv;

  clv = calloc(1, sizeof(*clv));
  clv->target = CONFIG_RAW;
  enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
  out = xmlOutputBufferCreateIO(mtev_config_log_write_xml,
                                mtev_config_log_close_xml,
                                clv, enc);
  mtev_conf_kansas_city_shuffle_undo(config_include_nodes, config_include_cnt);
  xmlSaveFormatFileTo(out, master_config, "utf8", 1);
  write_out_include_files(config_include_nodes, config_include_cnt);
  mtev_conf_kansas_city_shuffle_redo(config_include_nodes, config_include_cnt);
  if(clv->encoded != CONFIG_RAW) {
    mtevL(mtev_error, "Error logging configuration\n");
    if(clv->buff) free(clv->buff);
    free(clv);
    return NULL;
  }
  rv = clv->buff;
  *len = clv->len;
  free(clv);
  return rv;
}

int
mtev_conf_write_log() {
  static u_int32_t last_write_gen = 0;
  static mtev_log_stream_t config_log = NULL;
  struct timeval __now;
  xmlOutputBufferPtr out;
  xmlCharEncodingHandlerPtr enc;
  struct config_line_vstr *clv;
  mtev_boolean notify_only = mtev_false;
  const char *v;
  SETUP_LOG(config, return -1);
  if(!N_L_S_ON(config_log)) return 0;

  v = mtev_log_stream_get_property(config_log, "notify_only");
  if(v && (!strcmp(v, "on") || !strcmp(v, "true"))) notify_only = mtev_true;

  /* We know we haven't changed */
  if(last_write_gen == __config_gen) return 0;
  gettimeofday(&__now, NULL);

  if(notify_only) {
    mtevL(config_log, "n\t%lu.%03lu\t%d\t\n",
          (unsigned long int)__now.tv_sec,
          (unsigned long int)__now.tv_usec / 1000UL, 0);
    last_write_gen = __config_gen;
    return 0;
  }

  clv = calloc(1, sizeof(*clv));
  clv->target = CONFIG_B64;
  enc = xmlGetCharEncodingHandler(XML_CHAR_ENCODING_UTF8);
  out = xmlOutputBufferCreateIO(mtev_config_log_write_xml,
                                mtev_config_log_close_xml,
                                clv, enc);
  mtev_conf_kansas_city_shuffle_undo(config_include_nodes, config_include_cnt);
  xmlSaveFormatFileTo(out, master_config, "utf8", 1);
  write_out_include_files(config_include_nodes, config_include_cnt);
  mtev_conf_kansas_city_shuffle_redo(config_include_nodes, config_include_cnt);
  if(clv->encoded != CONFIG_B64) {
    mtevL(mtev_error, "Error logging configuration\n");
    if(clv->buff) free(clv->buff);
    free(clv);
    return -1;
  }
  mtevL(config_log, "n\t%lu.%03lu\t%d\t%.*s\n",
        (unsigned long int)__now.tv_sec,
        (unsigned long int)__now.tv_usec / 1000UL, clv->raw_len,
        clv->len, clv->buff);
  free(clv->buff);
  free(clv);
  last_write_gen = __config_gen;
  return 0;
}

struct log_rotate_crutch {
  mtev_log_stream_t ls;
  int seconds;
  int retain_seconds;
  size_t max_size;
  ssize_t retain_size;
};

static int
mtev_conf_log_cull(eventer_t e, int mask, void *closure,
                   struct timeval *now) {
  struct log_rotate_crutch *lrc = closure;
  if(!(mask & EVENTER_ASYNCH_WORK)) return 0;
  mtev_log_stream_cull(lrc->ls, lrc->retain_seconds, lrc->retain_size);
  return 0;
}
static void
schedule_background_log_cull(struct log_rotate_crutch *lrc) {
  eventer_t e;
  if(lrc->retain_size < 0 && lrc->retain_seconds < 0) return;
  e = eventer_alloc();
  e->closure = lrc;
  e->callback = mtev_conf_log_cull;
  e->mask = EVENTER_ASYNCH;
  eventer_add(e);
}
static int
mtev_conf_log_rotate_size(eventer_t e, int mask, void *closure,
                          struct timeval *now) {
  struct log_rotate_crutch *lrc = closure;
  if(mtev_log_stream_written(lrc->ls) > lrc->max_size) {
    mtev_log_stream_rename(lrc->ls, MTEV_LOG_RENAME_AUTOTIME);
    mtev_log_stream_reopen(lrc->ls);
    schedule_background_log_cull(lrc);
  }
  /* Yes the 5 is arbitrary, but this is cheap */
  eventer_add_in_s_us(mtev_conf_log_rotate_size, closure, 5, 0);
  return 0;
}
static int
mtev_conf_log_rotate_time(eventer_t e, int mask, void *closure,
                          struct timeval *now) {
  struct timeval lnow;
  eventer_t newe;
  struct log_rotate_crutch *lrc = closure;

  if(now) {
    mtev_log_stream_rename(lrc->ls, MTEV_LOG_RENAME_AUTOTIME);
    mtev_log_stream_reopen(lrc->ls);
    schedule_background_log_cull(lrc);
  }
  
  newe = eventer_alloc();
  newe->closure = closure;
  if(!now) { gettimeofday(&lnow, NULL); now = &lnow; }
  if(e)
    memcpy(&newe->whence, &e->whence, sizeof(newe->whence));
  else if(now) {
    memcpy(&newe->whence, now, sizeof(newe->whence));
    newe->whence.tv_sec = (newe->whence.tv_sec / lrc->seconds) * lrc->seconds;
  }
  newe->whence.tv_sec += lrc->seconds;
  newe->mask = EVENTER_TIMER;
  newe->callback = mtev_conf_log_rotate_time;
  eventer_add(newe);
  return 0;
}
int
mtev_conf_log_init_rotate(const char *toplevel, mtev_boolean validate) {
  int i, cnt = 0, max_time, max_size, retain_seconds = -1, retain_size = -1, rv = 0;
  mtev_conf_section_t *log_configs;
  char path[256];

  snprintf(path, sizeof(path), "/%s/logs//log|/%s/include/logs//log", toplevel, toplevel);
  log_configs = mtev_conf_get_sections(NULL, path, &cnt);
  mtevL(mtev_debug, "Found %d %s stanzas\n", cnt, path);
  for(i=0; i<cnt; i++) {
    mtev_log_stream_t ls;
    char name[256];

    if(!mtev_conf_get_stringbuf(log_configs[i],
                                "ancestor-or-self::node()/@name",
                                name, sizeof(name))) {
      mtevL(mtev_error, "log section %d does not have a name attribute\n", i+1);
      if(validate) { rv = -1; break; }
      else exit(-2);
    }
    ls = mtev_log_stream_find(name);
    if(!ls) continue;

    if(mtev_conf_get_int(log_configs[i],    
                         "ancestor-or-self::node()/@rotate_seconds",
                         &max_time) && max_time) {
      struct log_rotate_crutch *lrc;
      if(max_time < 600) {
        fprintf(stderr, "rotate_seconds must be >= 600s (10 minutes)\n");
        if(validate) { rv = -1; break; }
        else exit(-2);
      }
      mtev_conf_get_int(log_configs[i],
                        "ancestor-or-self::node()/@retain_seconds",
                        &retain_seconds);
      if(!validate) {
        lrc = calloc(1, sizeof(*lrc));
        lrc->ls = ls;
        lrc->seconds = max_time;
        lrc->retain_size = -1;
        lrc->retain_seconds = retain_seconds;
        mtev_conf_log_rotate_time(NULL, EVENTER_TIMER, lrc, NULL);
      }
    }

    if(mtev_conf_get_int(log_configs[i],    
                         "ancestor-or-self::node()/@rotate_bytes",
                         &max_size) && max_size) {
      struct log_rotate_crutch *lrc;
      if(max_size < 102400) {
        fprintf(stderr, "rotate_bytes must be >= 102400 (100k)\n");
        if(validate) { rv = -1; break; }
        else exit(-2);
      }
      mtev_conf_get_int(log_configs[i],
                        "ancestor-or-self::node()/@retain_bytes",
                        &retain_size);
      if(!validate) {
        lrc = calloc(1, sizeof(*lrc));
        lrc->ls = ls;
        lrc->max_size = max_size;
        lrc->retain_seconds = -1;
        lrc->retain_size = retain_size;
        mtev_conf_log_rotate_size(NULL, EVENTER_TIMER, lrc, NULL);
      }
    }
  }
  free(log_configs);
  return rv;
}
void
mtev_conf_log_init(const char *toplevel,
                   const char *drop_to_user, const char *drop_to_group) {
  int i, cnt = 0, o, ocnt = 0;
  mtev_conf_section_t *log_configs, *outlets;
  char path[256], user[128], group[128];

  snprintf(user, sizeof(user), "%d", getuid());
  snprintf(group, sizeof(group), "%d", getgid());
  if(!drop_to_user) drop_to_user = user;
  if(!drop_to_group) drop_to_group = group;
  if(mtev_security_usergroup(drop_to_user, drop_to_group, mtev_true)) {
    mtevL(mtev_stderr, "Failed to drop privileges, exiting.\n");
    exit(-1);
  }

  snprintf(path, sizeof(path), "/%s/logs//log|/%s/include/logs//log", toplevel, toplevel);
  log_configs = mtev_conf_get_sections(NULL, path, &cnt);
  mtevL(mtev_debug, "Found %d %s stanzas\n", cnt, path);
  for(i=0; i<cnt; i++) {
    int flags;
    mtev_log_stream_t ls;
    char name[256], type[256], path[256];
    mtev_hash_table *config;
    mtev_boolean disabled, debug, timestamps, facility;

    if(!mtev_conf_get_stringbuf(log_configs[i],
                                "ancestor-or-self::node()/@name",
                                name, sizeof(name))) {
      mtevL(mtev_error, "log section %d does not have a name attribute\n", i+1);
      exit(-1);
    }
    if(!mtev_conf_get_stringbuf(log_configs[i],
                                "ancestor-or-self::node()/@type",
                                type, sizeof(type))) {
      type[0] = '\0';
    }
    if(!mtev_conf_get_stringbuf(log_configs[i],
                                "ancestor-or-self::node()/@path",
                                path, sizeof(path))) {
      path[0] = '\0';
    }
    config = mtev_conf_get_hash(log_configs[i],
                                "ancestor-or-self::node()/config");
    ls = mtev_log_stream_new(name, type[0] ? type : NULL,
                             path[0] ? path : NULL, NULL, config);
    if(!ls) {
      fprintf(stderr, "Error configuring log: %s[%s:%s]\n", name, type, path);
      exit(-1);
    }

    flags = mtev_log_stream_get_flags(ls);
    if(mtev_conf_get_boolean(log_configs[i],
                             "ancestor-or-self::node()/@disabled",
                             &disabled)) {
      if(disabled) flags &= ~MTEV_LOG_STREAM_ENABLED;
      else         flags |= MTEV_LOG_STREAM_ENABLED;
    }
    if(mtev_conf_get_boolean(log_configs[i],
                             "ancestor-or-self::node()/@debug",
                             &debug)) {
      if(debug) flags |= MTEV_LOG_STREAM_DEBUG;
      else      flags &= ~MTEV_LOG_STREAM_DEBUG;
    }
    if(mtev_conf_get_boolean(log_configs[i],
                             "ancestor-or-self::node()/@timestamps",
                             &timestamps)) {
      if(timestamps) flags |= MTEV_LOG_STREAM_TIMESTAMPS;
      else           flags &= ~MTEV_LOG_STREAM_TIMESTAMPS;
    }
    if(mtev_conf_get_boolean(log_configs[i],
                             "ancestor-or-self::node()/@facility",
                             &facility)) {
      if(facility) flags |= MTEV_LOG_STREAM_FACILITY;
      else         flags &= ~MTEV_LOG_STREAM_FACILITY;
    }
    mtev_log_stream_set_flags(ls, flags);

    outlets = mtev_conf_get_sections(log_configs[i],
                                     "ancestor-or-self::node()/outlet", &ocnt);
    mtevL(mtev_debug, "Found %d outlets for log '%s'\n", ocnt, name);

    for(o=0; o<ocnt; o++) {
      mtev_log_stream_t outlet;
      char oname[256];
      mtev_conf_get_stringbuf(outlets[o], "@name",
                              oname, sizeof(oname));
      outlet = mtev_log_stream_find(oname);
      if(!outlet) {
        fprintf(stderr, "Cannot find outlet '%s' for %s[%s:%s]\n", oname,
              name, type, path);
        exit(-1);
      }
      else
        mtev_log_stream_add_stream(ls, outlet);
    }
    if(outlets) free(outlets);
  }
  if(log_configs) free(log_configs);
  if(mtev_conf_log_init_rotate(toplevel, mtev_true)) exit(-1);

  if(mtev_security_usergroup(user, group, mtev_true)) {
    mtevL(mtev_stderr, "Failed to regain privileges, exiting.\n");
    exit(-1);
  }
}

void
mtev_conf_security_init(const char *toplevel, const char *user,
                        const char *group, const char *chrootpath) {
  int i, ccnt = 0;
  mtev_conf_section_t *secnode, *caps;
  char path[256];
  char username[128], groupname[128], chrootpathname[PATH_MAX];

  snprintf(path, sizeof(path), "/%s/security|/%s/include/security",
           toplevel, toplevel);
  secnode = mtev_conf_get_section(NULL, path);

  if(user) {
    strlcpy(username, user, sizeof(username));
    user = username;
  }
  else if(secnode &&
          mtev_conf_get_stringbuf(secnode, "self::node()/@user",
                                  username, sizeof(username))) {
    user = username;
  }
  if(group) {
    strlcpy(groupname, group, sizeof(groupname));
    group = groupname;
  }
  else if(secnode &&
          mtev_conf_get_stringbuf(secnode, "self::node()/@group",
                                  groupname, sizeof(groupname))) {
    group = groupname;
  }
  if(chrootpath) {
    strlcpy(chrootpathname, chrootpath, sizeof(chrootpathname));
    chrootpath = chrootpathname;
  }
  else if(secnode &&
          mtev_conf_get_stringbuf(secnode, "self::node()/@chrootpath",
                                  chrootpathname, sizeof(chrootpathname))) {
    chrootpath = chrootpathname;
  }

  /* chroot first */
  if(chrootpath && mtev_security_chroot(chrootpath)) {
    mtevL(mtev_stderr, "Failed to chroot(), exiting.\n");
    exit(2);
  }

  caps = mtev_conf_get_sections(secnode,
                                "self::node()//capabilities//capability", &ccnt);
  mtevL(mtev_debug, "Found %d capabilities.\n", ccnt);

  for(i=0; i<ccnt; i++) {
    /* articulate capabilities */
    char platform[32], captype_str[32];
    char *capstring;
    mtev_security_captype_t captype;
    if(mtev_conf_get_stringbuf(caps[i], "ancestor-or-self::node()/@platform",
                               platform, sizeof(platform)) &&
       strcasecmp(platform, CAP_PLATFORM)) {
      mtevL(mtev_debug, "skipping cap for platform %s\n", platform);
      continue;
    }

    captype_str[0] = '\0'; 
    mtev_conf_get_stringbuf(caps[i], "ancestor-or-self::node()/@type",
                            captype_str, sizeof(captype_str));
    if(!strcasecmp(captype_str, "permitted"))
      captype = MTEV_SECURITY_CAP_PERMITTED;
    else if(!strcasecmp(captype_str, "effective"))
      captype = MTEV_SECURITY_CAP_EFFECTIVE;
    else if(!strcasecmp(captype_str, "inheritable"))
      captype = MTEV_SECURITY_CAP_INHERITABLE;
    else {
      mtevL(mtev_error, "Unsupport capability type: '%s'\n", captype_str);
      exit(2);
    }

    capstring = NULL;
    mtev_conf_get_string(caps[i], "self::node()", &capstring);
    if(capstring) {
      if(mtev_security_setcaps(captype, capstring) != 0) {
        mtevL(mtev_error, "Failed to set security capabilities: %s / %s\n",
              captype_str, capstring);
        exit(2);
      }
      free(capstring);
    }
  }
  if(caps) free(caps);

  /* drop uid/gid last */
  if(mtev_security_usergroup(user, group, mtev_false)) { /* no take backs */
    mtevL(mtev_stderr, "Failed to drop privileges, exiting.\n");
    exit(2);
  }
}

static void
conf_t_userdata_free(void *data) {
  mtev_conf_t_userdata_t *info = data;
  if(info) {
    if(info->path) free(info->path);
    free(info);
  }
}

static int
mtev_console_state_conf_terminal(mtev_console_closure_t ncct,
                                 int argc, char **argv,
                                 mtev_console_state_t *state, void *closure) {
  mtev_conf_t_userdata_t *info;
  if(argc) {
    nc_printf(ncct, "extra arguments not expected.\n");
    return -1;
  }
  info = calloc(1, sizeof(*info));
  info->path = strdup("/");
  mtev_console_userdata_set(ncct, MTEV_CONF_T_USERDATA, info,
                            conf_t_userdata_free);
  mtev_console_state_push_state(ncct, state);
  mtev_console_state_init(ncct);
  return 0;
}
static int
mtev_console_config_section(mtev_console_closure_t ncct,
                            int argc, char **argv,
                            mtev_console_state_t *state, void *closure) {
  const char *err = "internal error";
  char *path, xpath[1024];
  mtev_conf_t_userdata_t *info;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL;
  xmlNodePtr node = NULL, newnode;
  vpsized_int delete = (vpsized_int)closure;

  mtev_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc != 1) {
    nc_printf(ncct, "requires one argument\n");
    return -1;
  }
  if(strchr(argv[0], '/')) {
    nc_printf(ncct, "invalid section name\n");
    return -1;
  }
  if(is_stopnode_name(argv[0])) {
    nc_printf(ncct, "%s is reserved.\n", argv[0]);
    return -1;
  }
  info = mtev_console_userdata_get(ncct, MTEV_CONF_T_USERDATA);
  if(!strcmp(info->path, "/")) {
    nc_printf(ncct, "manipulation of toplevel section disallowed\n");
    return -1;
  }

  if(delete) {
    /* We cannot delete if we have checks */
    snprintf(xpath, sizeof(xpath), "/%s%s/%s//check", root_node_name,
             info->path, argv[0]);
    pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
    if(!pobj || pobj->type != XPATH_NODESET ||
       !xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
      err = "cannot delete section, has checks";
      goto bad;
    }
    if(pobj) xmlXPathFreeObject(pobj);
  }

  snprintf(xpath, sizeof(xpath), "/%s%s/%s", root_node_name,
           info->path, argv[0]);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    err = "internal error: cannot detect section";
    goto bad;
  }
  if(!delete && !xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    if(xmlXPathNodeSetGetLength(pobj->nodesetval) == 1) {
      node = xmlXPathNodeSetItem(pobj->nodesetval, 0);
      free(info->path);
      info->path = strdup((char *)xmlGetNodePath(node) +
                          1 + strlen(root_node_name));
      goto cdout;
    }
    err = "cannot create section";
    goto bad;
  }
  if(delete && xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    err = "no such section";
    goto bad;
  }
  if(delete) {
    node = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
    if(node) {
      CONF_REMOVE(node);
      xmlUnlinkNode(node);
      mtev_conf_mark_changed();
    }
    return 0;
  }
  if(pobj) xmlXPathFreeObject(pobj);
  pobj = NULL;

  if(!strcmp(argv[0],"include")) {
    err = "include is a reserved section name";
    goto bad;
  }
  path = strcmp(info->path, "/") ? info->path : "";
  snprintf(xpath, sizeof(xpath), "/%s%s", root_node_name, path);
  pobj = xmlXPathEval((xmlChar *)xpath, xpath_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetGetLength(pobj->nodesetval) != 1) {
    err = "path invalid?";
    goto bad;
  }
  node = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if((newnode = xmlNewChild(node, NULL, (xmlChar *)argv[0], NULL)) != NULL) {
    mtev_conf_mark_changed();
    free(info->path);
    info->path = strdup((char *)xmlGetNodePath(newnode) + 1 +
                        strlen(root_node_name));
  }
  else {
    err = "failed to create section";
    goto bad;
  }
 cdout:
  if(pobj) xmlXPathFreeObject(pobj);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  nc_printf(ncct, "%s\n", err);
  return -1;
}

int
mtev_console_generic_show(mtev_console_closure_t ncct,
                          int argc, char **argv,
                          mtev_console_state_t *state, void *closure) {
  int i, cnt, titled = 0, cliplen = 0;
  const char *path = "", *basepath = NULL;
  char xpath[1024];
  mtev_conf_t_userdata_t *info = NULL;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL, current_ctxt;
  xmlDocPtr master_config = NULL;
  xmlNodePtr node = NULL;

  mtev_conf_xml_xpath(&master_config, &xpath_ctxt);
  if(argc > 1) {
    nc_printf(ncct, "too many arguments\n");
    return -1;
  }

  info = mtev_console_userdata_get(ncct, MTEV_CONF_T_USERDATA);
  if(info && info->path) path = basepath = info->path;
  if(!info && argc == 0) {
    nc_printf(ncct, "argument required when not in configuration mode\n");
    return -1;
  }

  if(argc == 1) path = argv[0];
  if(!basepath) basepath = path;

  /* { / } is a special case */
  if(!strcmp(basepath, "/")) basepath = "";
  if(!strcmp(path, "/")) path = "";

  if(!master_config) {
    nc_printf(ncct, "no config\n");
    return -1;
  }

  /* { / } is the only path that will end with a /
   * in XPath { / / * } means something _entirely different than { / * }
   * Ever notice how it is hard to describe xpath in C comments?
   */
  /* We don't want to show the root node */
  cliplen = strlen(root_node_name) + 2; /* /name/ */

  /* If we are in configuration mode
   * and we are without an argument or the argument is absolute,
   * clip the current path off */
  if(info && (argc == 0 || path[0] != '/')) cliplen += strlen(basepath);
  if(!path[0] || path[0] == '/') /* base only, or absolute path requested */
    snprintf(xpath, sizeof(xpath), "/%s%s/@*", root_node_name, path);
  else
    snprintf(xpath, sizeof(xpath), "/%s%s/%s/@*", root_node_name,
             basepath, path);

  current_ctxt = xpath_ctxt;
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    nc_printf(ncct, "no such object\n");
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  titled = 0;
  for(i=0; i<cnt; i++) {
    node = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(node->children && node->children == xmlGetLastChild(node) &&
      xmlNodeIsText(node->children)) {
      if(!titled++) nc_printf(ncct, "== Section Settings ==\n");
      nc_printf(ncct, "%s: %s\n", xmlGetNodePath(node) + cliplen,
                xmlXPathCastNodeToString(node->children));
    }
  }
  xmlXPathFreeObject(pobj);

  /* _shorten string_ turning last { / @ * } to { / * } */
  if(!path[0] || path[0] == '/') /* base only, or absolute path requested */
    snprintf(xpath, sizeof(xpath), "/%s%s/*", root_node_name, path);
  else
    snprintf(xpath, sizeof(xpath), "/%s%s/%s/*",
             root_node_name, basepath, path);
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET) {
    nc_printf(ncct, "no such object\n");
    goto bad;
  }
  cnt = xmlXPathNodeSetGetLength(pobj->nodesetval);
  titled = 0;
  for(i=0; i<cnt; i++) {
    node = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, i);
    if(!(node->children && node->children == xmlGetLastChild(node) &&
         xmlNodeIsText(node->children))) {
      if(!titled++) nc_printf(ncct, "== Subsections ==\n");
      nc_printf(ncct, "%s\n", xmlGetNodePath(node) + cliplen);
    }
  }
  xmlXPathFreeObject(pobj);
  return 0;
 bad:
  if(pobj) xmlXPathFreeObject(pobj);
  return -1;
}
int
mtev_console_config_cd(mtev_console_closure_t ncct,
                       int argc, char **argv,
                       mtev_console_state_t *state, void *closure) {
  const char *err = "internal error";
  char *path = NULL, xpath[1024];
  mtev_conf_t_userdata_t *info;
  xmlXPathObjectPtr pobj = NULL;
  xmlXPathContextPtr xpath_ctxt = NULL, current_ctxt;
  xmlNodePtr node = NULL;
  char *dest;

  mtev_conf_xml_xpath(NULL, &xpath_ctxt);
  if(argc != 1 && !closure) {
    nc_printf(ncct, "requires one argument\n");
    return -1;
  }
  dest = argc ? argv[0] : (char *)closure;
  info = mtev_console_userdata_get(ncct, MTEV_CONF_T_USERDATA);
  if(dest[0] == '/')
    snprintf(xpath, sizeof(xpath), "/%s%s", root_node_name, dest);
  else {
    snprintf(xpath, sizeof(xpath), "/%s%s/%s", root_node_name,
             info->path, dest);
  }
  if(xpath[strlen(xpath)-1] == '/') xpath[strlen(xpath)-1] = '\0';

  current_ctxt = xpath_ctxt;
  pobj = xmlXPathEval((xmlChar *)xpath, current_ctxt);
  if(!pobj || pobj->type != XPATH_NODESET ||
     xmlXPathNodeSetIsEmpty(pobj->nodesetval)) {
    err = "no such section";
    goto bad;
  }
  if(xmlXPathNodeSetGetLength(pobj->nodesetval) > 1) {
    err = "ambiguous section";
    goto bad;
  }

  node = (mtev_conf_section_t)xmlXPathNodeSetItem(pobj->nodesetval, 0);
  if(!node) {
    err = "internal XML error";
    goto bad;
  }
  if(is_stopnode(node)) {
    err = "reserved word";
    goto bad;
  }
  path = (char *)xmlGetNodePath(node);
  if(strlen(path) < strlen(root_node_name) + 1 ||
     strncmp(path + 1, root_node_name, strlen(root_node_name)) ||
     (path[strlen(root_node_name) + 1] != '/' &&
      path[strlen(root_node_name) + 1] != '\0')) {
    err = "new path outside out tree";
    goto bad;
  }
  free(info->path);
  if(!strcmp(path + 1, root_node_name))
    info->path = strdup("/");
  else {
    char *xmlpath = (char *)xmlGetNodePath(node);
    info->path = strdup(xmlpath + 1 +
                        strlen(root_node_name));
    free(xmlpath);
  }

  free(path);
  if(pobj) xmlXPathFreeObject(pobj);
  if(closure) mtev_console_state_pop(ncct, argc, argv, NULL, NULL);
  return 0;
 bad:
  if(path) free(path);
  if(pobj) xmlXPathFreeObject(pobj);
  nc_printf(ncct, "%s [%s]\n", err, xpath);
  return -1;
}

char *
conf_t_prompt(EditLine *el) {
  mtev_console_closure_t ncct;
  mtev_conf_t_userdata_t *info;
  static char *tl = "mtev(conf)# ";
  static char *pfmt = "mtev(conf:%s%s)# ";
  int path_len, max_len;

  el_get(el, EL_USERDATA, (void *)&ncct);
  if(!ncct) return tl;
  info = mtev_console_userdata_get(ncct, MTEV_CONF_T_USERDATA);
  if(!info) return tl;

  path_len = strlen(info->path);
  max_len = sizeof(info->prompt) - (strlen(pfmt) - 4 /* %s%s */) - 1 /* \0 */;
  if(path_len > max_len)
    snprintf(info->prompt, sizeof(info->prompt),
             pfmt, "...", info->path + path_len - max_len + 3 /* ... */);
  else
    snprintf(info->prompt, sizeof(info->prompt), pfmt, "", info->path);
  return info->prompt;
}

#define NEW_STATE(a) (a) = mtev_console_state_alloc()
#define ADD_CMD(a,cmd,func,ac,ss,c) \
  mtev_console_state_add_cmd((a), \
    NCSCMD(cmd, func, ac, ss, c))
#define DELEGATE_CMD(a,cmd,ac,ss) \
  mtev_console_state_add_cmd((a), \
    NCSCMD(cmd, mtev_console_state_delegate, ac, ss, NULL))

void mtev_console_conf_init() {
  mtev_console_state_t *tl, *_conf_state, *_conf_t_state,
                       *_write_state, *_unset_state;

  tl = mtev_console_state_initial();

  /* write <terimal|memory|file> */
  NEW_STATE(_write_state);
  ADD_CMD(_write_state, "terminal", mtev_conf_write_terminal, NULL, NULL, NULL);
  ADD_CMD(_write_state, "file", mtev_conf_write_file_console, NULL, NULL, NULL);
  /* write memory?  It's to a file, but I like router syntax */
  ADD_CMD(_write_state, "memory", mtev_conf_write_file_console, NULL, NULL, NULL);

  NEW_STATE(_unset_state);
  ADD_CMD(_unset_state, "section",
          mtev_console_config_section, NULL, NULL, (void *)1);

  NEW_STATE(_conf_t_state); 
  _conf_t_state->console_prompt_function = conf_t_prompt;
  mtev_console_state_add_cmd(_conf_t_state, &console_command_exit);

  ADD_CMD(_conf_t_state, "ls", mtev_console_generic_show, NULL, NULL, NULL);
  ADD_CMD(_conf_t_state, "cd", mtev_console_config_cd, NULL, NULL, NULL);
  ADD_CMD(_conf_t_state, "section",
          mtev_console_config_section, NULL, NULL, (void *)0);

  DELEGATE_CMD(_conf_t_state, "write",
               mtev_console_opt_delegate, _write_state);
  DELEGATE_CMD(_conf_t_state, "no", mtev_console_opt_delegate, _unset_state);

  NEW_STATE(_conf_state);
  ADD_CMD(_conf_state, "terminal",
          mtev_console_state_conf_terminal, NULL, _conf_t_state, NULL);

  ADD_CMD(tl, "configure",
          mtev_console_state_delegate, mtev_console_opt_delegate,
          _conf_state, NULL);
  ADD_CMD(tl, "write",
          mtev_console_state_delegate, mtev_console_opt_delegate,
          _write_state, NULL);
}

