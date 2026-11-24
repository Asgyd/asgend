#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "asgend_memory.h"
#include "internal.h"

#include <sqlite3.h>
#include <json-c/json.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

#define GRAPH_MAX_CONTENT (4U * 1024U * 1024U)
#define GRAPH_MAX_REFERENCE 1024U

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *path_lookup;
    sqlite3_stmt *edge_insert;
    uint64_t edges_added;
    uint64_t references_seen;
} GraphContext;

static void graph_error(char *out, size_t cap, const char *fmt, ...) {
    if (!out || !cap) return;
    va_list ap; va_start(ap, fmt); (void)vsnprintf(out, cap, fmt, ap); va_end(ap);
}

static bool has_scheme(const char *s) {
    if (!s) return true;
    if (!strncasecmp(s, "http:", 5) || !strncasecmp(s, "https:", 6) ||
        !strncasecmp(s, "mailto:", 7) || !strncasecmp(s, "data:", 5) ||
        !strncasecmp(s, "tel:", 4) || !strncasecmp(s, "javascript:", 11)) return true;
    return s[0] == '#' || (s[0] == '/' && s[1] == '/');
}

static char *normalize_reference(const char *source_path, const char *reference, bool root_relative) {
    if (!source_path || !reference || !*reference || strlen(reference) > GRAPH_MAX_REFERENCE || has_scheme(reference)) return NULL;
    char *ref = strdup(reference);
    if (!ref) return NULL;
    char *end = strpbrk(ref, "?#"); if (end) *end = '\0';
    while (*ref && isspace((unsigned char)*ref)) memmove(ref, ref + 1, strlen(ref));
    size_t len = strlen(ref);
    while (len && (isspace((unsigned char)ref[len-1]) || ref[len-1] == '>' || ref[len-1] == '"' || ref[len-1] == '\'')) ref[--len] = '\0';
    if (!len || has_scheme(ref)) { free(ref); return NULL; }
    for (size_t i=0;i<len;++i) if (ref[i] == '\\') ref[i] = '/';
    char joined[GRAPH_MAX_REFERENCE * 2U + 4U];
    if (ref[0] == '/') {
        if (!root_relative) { free(ref); return NULL; }
        (void)snprintf(joined, sizeof(joined), "%s", ref + 1);
    } else if (root_relative) {
        (void)snprintf(joined, sizeof(joined), "%s", ref);
    } else {
        const char *slash = strrchr(source_path, '/');
        size_t dir_len = slash ? (size_t)(slash - source_path) : 0U;
        if (dir_len + len + 2U > sizeof(joined)) { free(ref); return NULL; }
        if (dir_len) { memcpy(joined, source_path, dir_len); joined[dir_len] = '/'; memcpy(joined + dir_len + 1, ref, len + 1); }
        else memcpy(joined, ref, len + 1);
    }
    free(ref);
    char *out = calloc(strlen(joined) + 2U, 1);
    if (!out) return NULL;
    size_t out_len=0;
    char *work = strdup(joined);
    if (!work) { free(out); return NULL; }
    char *save=NULL;
    for (char *part=strtok_r(work, "/", &save); part; part=strtok_r(NULL,"/",&save)) {
        if (!strcmp(part,".") || !*part) continue;
        if (!strcmp(part,"..")) {
            if (!out_len) { free(work); free(out); return NULL; }
            while (out_len && out[out_len-1] != '/') --out_len;
            if (out_len) --out_len;
            out[out_len]='\0';
            continue;
        }
        if (out_len) out[out_len++]='/';
        size_t part_len=strlen(part); memcpy(out+out_len,part,part_len); out_len+=part_len; out[out_len]='\0';
    }
    free(work);
    if (!out_len) { free(out); return NULL; }
    return out;
}

static int lookup_target(GraphContext *ctx, const char *path, int64_t *target_id) {
    static const char *suffixes[] = {"", ".md", ".txt", ".rst", ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".js", ".ts", ".tsx", ".jsx", ".go", ".rs", ".json", ".yaml", ".yml", "/index.md", "/README.md"};
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    bool has_extension = strrchr(base, '.') != NULL;
    size_t tries = has_extension ? 1U : sizeof(suffixes)/sizeof(suffixes[0]);
    for (size_t i=0;i<tries;++i) {
        char candidate[GRAPH_MAX_REFERENCE * 2U + 32U];
        int n=snprintf(candidate,sizeof(candidate),"%s%s",path, has_extension ? "" : suffixes[i]);
        if (n<0 || (size_t)n>=sizeof(candidate)) continue;
        sqlite3_reset(ctx->path_lookup); sqlite3_clear_bindings(ctx->path_lookup);
        sqlite3_bind_text(ctx->path_lookup,1,candidate,-1,SQLITE_TRANSIENT);
        int rc=sqlite3_step(ctx->path_lookup);
        if(rc==SQLITE_ROW) { *target_id=sqlite3_column_int64(ctx->path_lookup,0); sqlite3_reset(ctx->path_lookup); return 0; }
        sqlite3_reset(ctx->path_lookup);
    }
    return 1;
}

static int insert_reference(GraphContext *ctx, int64_t source_id, const char *source_path,
                            const char *source_hash, const char *reference, const char *edge_type,
                            bool root_relative) {
    if (!reference || !*reference) return 0;
    ++ctx->references_seen;
    char *normalized=normalize_reference(source_path,reference,root_relative);
    if (!normalized) return 0;
    int64_t target_id=0; int found=lookup_target(ctx,normalized,&target_id);
    free(normalized);
    if (found != 0 || target_id == source_id) return 0;
    sqlite3_reset(ctx->edge_insert); sqlite3_clear_bindings(ctx->edge_insert);
    sqlite3_bind_int64(ctx->edge_insert,1,source_id);
    sqlite3_bind_int64(ctx->edge_insert,2,target_id);
    sqlite3_bind_text(ctx->edge_insert,3,edge_type,-1,SQLITE_STATIC);
    sqlite3_bind_text(ctx->edge_insert,4,reference,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(ctx->edge_insert,5,source_hash,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(ctx->edge_insert);
    if(rc!=SQLITE_DONE) return -1;
    if(sqlite3_changes(ctx->db)>0) ++ctx->edges_added;
    return 0;
}

static int scan_markdown(GraphContext *ctx,int64_t file_id,const char *path,const char *hash,const char *text) {
    for (const char *p=text; (p=strstr(p,"]("))!=NULL; ++p) {
        p+=2; while (*p && isspace((unsigned char)*p)) ++p;
        bool angle=(*p=='<'); if(angle) ++p;
        const char *start=p;
        while(*p && (angle ? *p!='>' : *p!=')' && !isspace((unsigned char)*p))) ++p;
        size_t n=(size_t)(p-start); if(n && n<=GRAPH_MAX_REFERENCE) {
            char ref[GRAPH_MAX_REFERENCE+1]; memcpy(ref,start,n); ref[n]='\0';
            if(insert_reference(ctx,file_id,path,hash,ref,"markdown_link",false)!=0) return -1;
        }
        if(!*p) break;
    }
    for (const char *p=text; (p=strstr(p,"[["))!=NULL; ++p) {
        p+=2; const char *start=p; while(*p && *p!=']' && *p!='|' && *p!='#') ++p;
        size_t n=(size_t)(p-start); if(n && n<=GRAPH_MAX_REFERENCE) {
            char ref[GRAPH_MAX_REFERENCE+1]; memcpy(ref,start,n); ref[n]='\0';
            if(insert_reference(ctx,file_id,path,hash,ref,"wiki_link",true)!=0) return -1;
        }
    }
    return 0;
}

static int scan_source_imports(GraphContext *ctx,int64_t file_id,const char *path,const char *hash,const char *text) {
    const char *line=text;
    while(*line) {
        const char *end=strchr(line,'\n'); if(!end) end=line+strlen(line);
        const char *p=line; while(p<end && isspace((unsigned char)*p)) ++p;
        if((size_t)(end-p)>=8 && !strncmp(p,"#include",8)) {
            p+=8; while(p<end && isspace((unsigned char)*p)) ++p;
            if(p<end && *p=='"') { const char *start=++p; while(p<end && *p!='"') ++p; size_t n=(size_t)(p-start); if(n && n<=GRAPH_MAX_REFERENCE) { char ref[GRAPH_MAX_REFERENCE+1]; memcpy(ref,start,n); ref[n]='\0'; if(insert_reference(ctx,file_id,path,hash,ref,"source_include",false)!=0)return -1; } }
        }
        /* Capture quoted relative imports in JS/TS/Python-like sources only. */
        bool import_line=(end-p>=7 && (!strncmp(p,"import ",7)||!strncmp(p,"from ",5))) || (end-p>=8 && memmem(p,(size_t)(end-p),"require(",8));
        if(import_line) {
            for(const char *q=p;q<end;++q) if(*q=='\''||*q=='"'||*q=='`') {
                const char *start=q+1;
                if(start<end && start[0]=='.' && (start+1>=end || start[1]=='.' || start[1]=='/')) {
                    const char *z=start; while(z<end && *z!=*q) ++z; size_t n=(size_t)(z-start);
                    if(n && n<=GRAPH_MAX_REFERENCE) { char ref[GRAPH_MAX_REFERENCE+1]; memcpy(ref,start,n); ref[n]='\0'; if(insert_reference(ctx,file_id,path,hash,ref,"source_import",false)!=0)return -1; }
                    q=z;
                }
            }
        }
        line=*end?end+1:end;
    }
    return 0;
}

int asgend_memory_graph(const char *root, FILE *out, char *error, size_t error_size) {
    if(error&&error_size) error[0]='\0';
    if(!root||!out){graph_error(error,error_size,"root and output stream are required");return -1;}
    char *canonical=NULL,*index_file=NULL; sqlite3 *db=NULL;
    if(asgend_memory_internal_open_index(root,&canonical,&db,&index_file,error,error_size)!=0) return -1;
    free(canonical);
    size_t lock_len=strlen(index_file)+16U; char *lock_path=malloc(lock_len);
    if(!lock_path){free(index_file);sqlite3_close(db);graph_error(error,error_size,"out of memory creating graph lock");return -1;}
    (void)snprintf(lock_path,lock_len,"%s.graph.lock",index_file); free(index_file);
    int lock_fd=open(lock_path,O_CREAT|O_RDWR|O_CLOEXEC,0600); free(lock_path);
    if(lock_fd<0){sqlite3_close(db);graph_error(error,error_size,"cannot open graph lock: %s",strerror(errno));return -1;}
    if(flock(lock_fd,LOCK_EX|LOCK_NB)!=0){close(lock_fd);sqlite3_close(db);graph_error(error,error_size,"another graph build is active for this project");return -1;}
    if(asgend_memory_internal_exec_sql(db,"BEGIN IMMEDIATE; DELETE FROM file_edges;",error,error_size)!=0){close(lock_fd);sqlite3_close(db);return -1;}
    GraphContext ctx={.db=db};
    const char *lookup="SELECT file_id FROM files WHERE relative_path=?1 AND deleted_at IS NULL ORDER BY file_id DESC LIMIT 1";
    const char *insert="INSERT OR IGNORE INTO file_edges(source_file_id,target_file_id,edge_type,reference_text,source_sha256) VALUES(?1,?2,?3,?4,?5)";
    sqlite3_stmt *files=NULL;
    if(sqlite3_prepare_v2(db,lookup,-1,&ctx.path_lookup,NULL)!=SQLITE_OK||sqlite3_prepare_v2(db,insert,-1,&ctx.edge_insert,NULL)!=SQLITE_OK||
       sqlite3_prepare_v2(db,"SELECT f.file_id,f.relative_path,f.sha256,c.content_text FROM files f JOIN file_contents c ON c.file_id=f.file_id AND c.source_sha256=f.sha256 WHERE f.deleted_at IS NULL AND f.sha256 IS NOT NULL AND c.extract_status IN ('extracted','truncated','parsed_partial') AND c.content_text IS NOT NULL",-1,&files,NULL)!=SQLITE_OK){graph_error(error,error_size,"prepare dependency graph: %s",sqlite3_errmsg(db));goto fail;}
    int rc;
    while((rc=sqlite3_step(files))==SQLITE_ROW){
        int64_t id=sqlite3_column_int64(files,0); const unsigned char *path=sqlite3_column_text(files,1),*hash=sqlite3_column_text(files,2),*content=sqlite3_column_text(files,3);
        int bytes=sqlite3_column_bytes(files,3);
        if(!path||!hash||!content||bytes<=0) continue;
        if((unsigned int)bytes>GRAPH_MAX_CONTENT) bytes=(int)GRAPH_MAX_CONTENT;
        char *text=malloc((size_t)bytes+1U); if(!text){graph_error(error,error_size,"out of memory processing graph source");goto fail;}
        memcpy(text,content,(size_t)bytes); text[bytes]='\0';
        const char *source_path=(const char *)path; char *path_copy=strdup(source_path); char *hash_copy=strdup((const char *)hash);
        if(!path_copy||!hash_copy){free(text);free(path_copy);free(hash_copy);graph_error(error,error_size,"out of memory copying graph source identity");goto fail;}
        int bad=scan_markdown(&ctx,id,path_copy,hash_copy,text)!=0||scan_source_imports(&ctx,id,path_copy,hash_copy,text)!=0;
        free(text);free(path_copy);free(hash_copy);
        if(bad){graph_error(error,error_size,"persist dependency graph edge: %s",sqlite3_errmsg(db));goto fail;}
    }
    if(rc!=SQLITE_DONE){graph_error(error,error_size,"read graph source files: %s",sqlite3_errmsg(db));goto fail;}
    sqlite3_finalize(files);files=NULL; sqlite3_finalize(ctx.path_lookup);ctx.path_lookup=NULL;sqlite3_finalize(ctx.edge_insert);ctx.edge_insert=NULL;
    if(asgend_memory_internal_exec_sql(db,"COMMIT;",error,error_size)!=0)goto cleanup_fail;
    json_object *summary=json_object_new_object(); if(!summary){graph_error(error,error_size,"out of memory creating graph summary");goto cleanup_fail;}
    sqlite3_stmt *stat=NULL; uint64_t nodes=0,edges=0;
    if(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM files WHERE deleted_at IS NULL),(SELECT count(*) FROM file_edges)",-1,&stat,NULL)==SQLITE_OK&&sqlite3_step(stat)==SQLITE_ROW){nodes=(uint64_t)sqlite3_column_int64(stat,0);edges=(uint64_t)sqlite3_column_int64(stat,1);} if(stat)sqlite3_finalize(stat);
    json_object_object_add(summary,"status",json_object_new_string("complete"));json_object_object_add(summary,"schema_version",json_object_new_int(5));json_object_object_add(summary,"nodes",json_object_new_uint64(nodes));json_object_object_add(summary,"edges",json_object_new_uint64(edges));json_object_object_add(summary,"references_seen",json_object_new_uint64(ctx.references_seen));json_object_object_add(summary,"edges_added",json_object_new_uint64(ctx.edges_added));
    const char *json=json_object_to_json_string_ext(summary,JSON_C_TO_STRING_PRETTY);int write_failed=!json||fprintf(out,"%s\n",json)<0;json_object_put(summary);
    close(lock_fd);sqlite3_close(db);if(write_failed){graph_error(error,error_size,"could not write graph summary");return -1;}return 0;
fail:
    if (files) sqlite3_finalize(files);
    if (ctx.path_lookup) sqlite3_finalize(ctx.path_lookup);
    if (ctx.edge_insert) sqlite3_finalize(ctx.edge_insert);
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
cleanup_fail:
    close(lock_fd);sqlite3_close(db);return -1;
}
