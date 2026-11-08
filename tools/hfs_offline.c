/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * HFSX binary catalog access, against Apple's published TN1150 format.
 * Catalog edits are rebuilt into a new B-tree in an unpublished image. This
 * avoids partially applied leaf splits, renamed threads and allocation maps.
 * There is no journal replay, host mount, executable mapping or guest CPU.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "hfs_offline.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#define sync_file _commit
#define file_no _fileno
#else
#include <unistd.h>
#define seek_file fseeko
#define tell_file ftello
#define sync_file fsync
#define file_no fileno
#endif
#ifndef ENOTSUP
#define ENOTSUP EINVAL
#endif
#define MAX_RECORDS 200000u
#define MAX_TREE (64u * 1024u * 1024u)
#define MAX_DIRTY (64u * 1024u * 1024u)
#define MAC_EPOCH UINT64_C(2082844800)
#define DIR_MODE 0040000u
#define FILE_MODE 0100000u
#define LINK_MODE 0120000u
#define TYPE_MODE 0170000u

typedef struct record {
    uint8_t *key, *value, *data;
    uint16_t key_length, value_length;
    size_t capacity;
    bool removed, dirty;
} record;
struct hfs_offline {
    FILE *source;
    uint64_t image_size;
    uint8_t header[512], tree_header[106];
    uint32_t block_size, block_count, next_id;
    uint16_t node_size;
    record **records;
    size_t count, capacity, dirty_bytes;
    /* Read-only extent overflow records, in on-disk key order. */
    record **extents;
    size_t extent_count;
    record **attributes;
    size_t attribute_count;
    uint8_t attribute_header[106];
    uint16_t attribute_node_size;
    bool dirty, failed;
};
static int audit_allocations(hfs_offline *v);
static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t r32(const uint8_t *p) { return ((uint32_t)r16(p)<<16)|r16(p+2); }
static uint64_t r64(const uint8_t *p) { return ((uint64_t)r32(p)<<32)|r32(p+4); }
static void w16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static void w32(uint8_t *p,uint32_t n) { w16(p,(uint16_t)(n>>16)); w16(p+2,(uint16_t)n); }
static void w64(uint8_t *p,uint64_t n) { w32(p,(uint32_t)(n>>32)); w32(p+4,(uint32_t)n); }
static uint32_t now_mac(void) {
    int64_t t=(int64_t)time(NULL)+(int64_t)MAC_EPOCH;
    return t<0?0:t>UINT32_MAX?UINT32_MAX:(uint32_t)t;
}
static int io_at(FILE *f,uint64_t size,uint64_t off,void *bytes,size_t count,bool write) {
    if (off>size || count>size-off || off>INT64_MAX) return EIO;
    if (seek_file(f,(int64_t)off,SEEK_SET)) return errno?errno:EIO;
    if (!count) return 0;
    size_t n=write?fwrite(bytes,1,count,f):fread(bytes,1,count,f);
    return n==count?0:EIO;
}
static uint32_t object_id(const record *r) {
    uint16_t type=r16(r->value);
    return type==1 || type==2?r32(r->value+8):0;
}
static record *by_id(hfs_offline *v,uint32_t id) {
    for (size_t i=0;i<v->count;i++) if (object_id(v->records[i])==id) return v->records[i];
    return NULL;
}
static uint32_t mode_of(const record *r) {
    uint32_t mode=r16(r->value+42);
    if (!(mode&TYPE_MODE)) mode|=r16(r->value)==1?DIR_MODE:FILE_MODE;
    return mode;
}
static int fork_inline(hfs_offline *v,const uint8_t *fork) {
    uint64_t covered=0;
    for (unsigned i=0;i<8;i++) {
        uint32_t start=r32(fork+16+i*8),n=r32(fork+20+i*8);
        if (!n) { if (start) return EIO; continue; }
        if (!start || start>=v->block_count || n>v->block_count-start) return EIO;
        covered+=n;
    }
    if (covered!=r32(fork+12)) return ENOTSUP;
    return r64(fork)>covered*v->block_size?EIO:0;
}
static int fork_io(hfs_offline *v,FILE *file,const uint8_t *fork,uint32_t id,
                   uint8_t type,uint64_t offset,void *bytes,size_t count,bool write) {
    if (offset>r64(fork) || count>r64(fork)-offset) return EIO;
    const uint8_t *ext=fork+16;
    uint64_t logical=0;
    uint32_t blocks=0;
    size_t done=0,steps=0;
    while (done<count) {
        uint32_t before=blocks;
        for (unsigned i=0;i<8 && done<count;i++) {
            uint32_t start=r32(ext+i*8),n=r32(ext+i*8+4);
            if (!n) continue;
            if (!start || start>=v->block_count || n>v->block_count-start ||
                n>r32(fork+12)-blocks) return EIO;
            uint64_t length=(uint64_t)n*v->block_size;
            if (offset<logical+length) {
                uint64_t delta=offset>logical?offset-logical:0;
                size_t take=(size_t)((length-delta)<count-done?length-delta:count-done);
                int e=io_at(file,v->image_size,(uint64_t)start*v->block_size+delta,
                            (uint8_t *)bytes+done,take,write);
                if (e) return e;
                done+=take; offset+=take;
            }
            logical+=length; blocks+=n;
        }
        if (done==count) break;
        if (blocks==before || ++steps>v->extent_count) return EIO;
        ext=NULL;
        for (size_t i=0;i<v->extent_count;i++) {
            record *r=v->extents[i];
            if (r->key[2]==type && r32(r->key+4)==id && r32(r->key+8)==blocks) {
                ext=r->value; break;
            }
        }
        if (!ext) return EIO;
    }
    return 0;
}
static void free_record(record *r) { if (r) { free(r->key); free(r->value); free(r->data); free(r); } }
static int add_record(hfs_offline *v,record *r) {
    if (v->count>=MAX_RECORDS) return EFBIG;
    if (v->count==v->capacity) {
        size_t n=v->capacity?v->capacity*2:128;
        record **p=(record **)realloc(v->records,n*sizeof(*p));
        if (!p) return ENOMEM;
        v->records=p; v->capacity=n;
    }
    v->records[v->count++]=r; return 0;
}
static int key_compare(const void *aa,const void *bb) {
    const record *a=*(const record *const *)aa,*b=*(const record *const *)bb;
    uint32_t ap=r32(a->key+2),bp=r32(b->key+2);
    if (ap!=bp) return ap<bp?-1:1;
    size_t an=r16(a->key+6),bn=r16(b->key+6),n=an<bn?an:bn;
    int c=memcmp(a->key+8,b->key+8,n*2);
    return c?c:an<bn?-1:an>bn?1:0;
}
static int load_tree(hfs_offline *v,const uint8_t *fork,unsigned kind) {
    bool extent=kind==1,attribute=kind==2;
    uint32_t file_id=extent?3:attribute?8:4;
    uint8_t head[120];
    if (!r64(fork)) return kind?0:EIO;
    int e=fork_inline(v,fork); if (e) return e;
    e=fork_io(v,v->source,fork,file_id,0,0,head,sizeof head,false); if (e) return e;
    uint16_t size=r16(head+32);
    uint32_t total=r32(head+36),leaf=r32(head+24),last=r32(head+28),expected=r32(head+20);
    if (head[8]!=1 || size<512 || size>32768 || (size&(size-1)) || !total ||
        (uint64_t)total*size>r64(fork) || (uint64_t)total*size>MAX_TREE || expected>MAX_RECORDS) return ENOTSUP;
    if (!kind) {
        if (head[51]!=0xbc) return ENOTSUP; /* binary HFSX, never guess collation */
        v->node_size=size; memcpy(v->tree_header,head+14,106);
    } else if (attribute) {
        v->attribute_node_size=size; memcpy(v->attribute_header,head+14,106);
    }
    uint8_t *node=(uint8_t *)malloc(size),*visited=(uint8_t *)calloc(total,1);
    if (!node || !visited) { free(node); free(visited); return ENOMEM; }
    uint32_t previous=0,seen=0;
    while (leaf) {
        if (leaf>=total || visited[leaf]) { e=EIO; break; }
        visited[leaf]=1;
        e=fork_io(v,v->source,fork,file_id,0,(uint64_t)leaf*size,node,size,false); if (e) break;
        uint16_t count=r16(node+10);
        if (node[8]!=255 || node[9]!=1 || r32(node+4)!=previous || count>(size-16)/4) { e=EIO; break; }
        for (unsigned i=0;i<count;i++) {
            uint16_t start=r16(node+size-2*(i+1)),end=r16(node+size-2*(i+2));
            if (start<14 || end<=start || end>size-2*(count+1) || end-start<4) { e=EIO; break; }
            uint16_t key=(uint16_t)(r16(node+start)+2);
            if (key<8 || key>end-start || (key&1)) { e=EIO; break; }
            const uint8_t *value=node+start+key;
            uint16_t val=(uint16_t)(end-start-key);
            if (extent) {
                if (key!=12 || val!=64) { e=EIO; break; }
            } else if (attribute) {
                if (key<14 || r16(node+start+12)>127 || key!=14+2*r16(node+start+12) || val<4) { e=EIO; break; }
                uint32_t type=r32(value);
                if (type==0x10) {
                    if (val<16 || r32(value+12)>val-16u) { e=EIO; break; }
                } else if ((type==0x20 && val==88) || (type==0x30 && val==72)) {
                    /* External attribute extents are retained, but deleting a
                     * file carrying one is currently a bounded refusal. */
                } else { e=ENOTSUP; break; }
            } else {
                if (key<8 || r16(node+start+6)>255 || key!=8+2*r16(node+start+6) || val<2) { e=EIO; break; }
                uint16_t type=r16(value),required=type==1?88:type==2?248:0;
                if (required) { if (val!=required) { e=EIO; break; } }
                else if ((type!=3 && type!=4) || val<10 || r16(value+8)>255 || val!=10+2*r16(value+8)) { e=EIO; break; }
            }
            record *r=(record *)calloc(1,sizeof *r);
            if (!r) { e=ENOMEM; break; }
            r->key=(uint8_t *)malloc(key); r->value=(uint8_t *)malloc(val);
            if (!r->key || !r->value) { free_record(r); e=ENOMEM; break; }
            r->key_length=key; r->value_length=val;
            memcpy(r->key,node+start,key); memcpy(r->value,value,val);
            e=add_record(v,r); if (e) { free_record(r); break; }
            if (!kind && v->count>1 && key_compare(&v->records[v->count-2],&r)>=0) { e=EIO; break; }
            seen++;
        }
        if (e) break;
        previous=leaf; leaf=r32(node);
    }
    if (!e && (previous!=last || seen!=expected)) e=EIO;
    free(node); free(visited); return e;
}
int hfs_offline_open(const char *path,hfs_offline **out) {
    if (!path || !out) return EINVAL;
    *out=NULL;
    hfs_offline *v=(hfs_offline *)calloc(1,sizeof *v); if (!v) return ENOMEM;
    v->source=fopen(path,"rb"); int e=0;
    if (!v->source) { e=errno; goto fail; }
    if (seek_file(v->source,0,SEEK_END)) { e=EIO; goto fail; }
    int64_t length=tell_file(v->source);
    if (length<2048) { e=EIO; goto fail; } v->image_size=(uint64_t)length;
    e=io_at(v->source,v->image_size,1024,v->header,512,false); if (e) goto fail;
    if (r16(v->header)!=0x4858 || r16(v->header+2)!=5 || (r32(v->header+4)&0x2000)) { e=ENOTSUP; goto fail; }
    v->block_size=r32(v->header+40); v->block_count=r32(v->header+44); v->next_id=r32(v->header+64);
    if (v->block_size<512 || v->block_size>1048576 || (v->block_size&(v->block_size-1)) ||
        v->block_count<16 || (uint64_t)v->block_count*v->block_size!=v->image_size ||
        v->next_id<16 || r64(v->header+112)>(16u*1024u*1024u)) { e=ENOTSUP; goto fail; }
    e=fork_inline(v,v->header+112); if (e) goto fail;
    if (r64(v->header+112)<((uint64_t)v->block_count+7)/8) { e=EIO; goto fail; }
    e=load_tree(v,v->header+192,1); if (e) goto fail;
    v->extents=v->records; v->extent_count=v->count; v->records=NULL; v->count=0; v->capacity=0;
    e=load_tree(v,v->header+352,2); if (e) goto fail;
    v->attributes=v->records; v->attribute_count=v->count; v->records=NULL; v->count=0; v->capacity=0;
    e=load_tree(v,v->header+272,0); if (e) goto fail;
    /* Prevent wraparound and reuse of an existing CNID on a stale header. */
    for (size_t i=0;i<v->count;i++) {
        uint32_t id=object_id(v->records[i]);
        if (id==UINT32_MAX) { e=ENOSPC; goto fail; }
        if (id>=v->next_id) v->next_id=id+1;
    }
    e=audit_allocations(v); if (e) goto fail;
    *out=v; return 0;
fail: hfs_offline_close(v); return e?e:EIO;
}
void hfs_offline_close(hfs_offline *v) {
    if (!v) return;
    if (v->source) fclose(v->source);
    for (size_t i=0;i<v->count;i++) free_record(v->records[i]);
    for (size_t i=0;i<v->extent_count;i++) free_record(v->extents[i]);
    for (size_t i=0;i<v->attribute_count;i++) free_record(v->attributes[i]);
    free(v->records); free(v->extents); free(v->attributes); free(v);
}
bool hfs_offline_dirty(const hfs_offline *v) { return v && v->dirty; }
static void info(const record *r,hfs_offline_info *out) {
    memset(out,0,sizeof *out); out->id=object_id(r); out->parent=r32(r->key+2);
    out->mode=mode_of(r); out->uid=r32(r->value+32); out->gid=r32(r->value+36);
    out->modified=(int64_t)r32(r->value+16)-(int64_t)MAC_EPOCH;
    out->size=r16(r->value)==2?r64(r->value+88):0;
    out->name_length=r16(r->key+6);
    for (size_t i=0;i<out->name_length;i++) out->name[i]=r16(r->key+8+2*i);
}
int hfs_offline_stat(hfs_offline *v,uint32_t id,hfs_offline_info *out) {
    if (!v || !out || v->failed) return EIO;
    record *r=by_id(v,id); if (!r) return ENOENT; info(r,out); return 0;
}
static bool name_matches(const record *r,uint32_t parent,const uint16_t *name,size_t length) {
    if (r->removed || !object_id(r) || r32(r->key+2)!=parent || r16(r->key+6)!=length) return false;
    for (size_t i=0;i<length;i++) if (r16(r->key+8+i*2)!=name[i]) return false;
    return true;
}
int hfs_offline_lookup(hfs_offline *v,uint32_t parent,const uint16_t *name,size_t length,hfs_offline_info *out) {
    if (!v || !name || !out || length>255 || v->failed) return EINVAL;
    for (size_t i=0;i<v->count;i++) if (name_matches(v->records[i],parent,name,length)) { info(v->records[i],out); return 0; }
    return ENOENT;
}
int hfs_offline_next(hfs_offline *v,uint32_t parent,size_t *cursor,hfs_offline_info *out) {
    if (!v || !cursor || !out || v->failed) return EINVAL;
    while (*cursor<v->count) {
        record *r=v->records[(*cursor)++];
        if (!r->removed && object_id(r) && r32(r->key+2)==parent) { info(r,out); return 0; }
    }
    return ENOENT;
}
int hfs_offline_read(hfs_offline *v,uint32_t id,uint64_t offset,void *bytes,size_t length,size_t *done) {
    if (!v || !done || (!bytes && length) || v->failed) return EINVAL;
    *done=0; record *r=by_id(v,id); if (!r) return ENOENT;
    if (r16(r->value)!=2) return EISDIR;
    uint64_t size=r64(r->value+88); if (offset>=size) return 0;
    size_t n=(size_t)(size-offset<length?size-offset:length);
    int e=0;
    if (r->dirty || r->removed) { if (n) memcpy(bytes,r->data+(size_t)offset,n); }
    else e=fork_io(v,v->source,r->value+88,id,0,offset,bytes,n,false);
    if (!e) *done=n;
    return e;
}
static int buffer_file(hfs_offline *v,record *r,uint64_t length) {
    if (r16(r->value)!=2) return EISDIR;
    if ((mode_of(r)&TYPE_MODE)!=FILE_MODE && (mode_of(r)&TYPE_MODE)!=LINK_MODE) return ENOTSUP;
    /* Finder hard-link stubs are not ordinary zero-length files. */
    if (!memcmp(r->value+48,"hlnk",4) || !memcmp(r->value+48,"fdrp",4)) return ENOTSUP;
    int e=fork_inline(v,r->value+88); if (e && !r->dirty) return e;
    uint64_t old=r64(r->value+88),want=length>old?length:old;
    if (want>MAX_DIRTY || want>MAX_DIRTY-(v->dirty_bytes-r->capacity)) return ENOSPC;
    if (want>r->capacity || !r->dirty) {
        size_t cap=(size_t)want;
        uint8_t *p=(uint8_t *)realloc(r->data,cap?cap:1); if (!p) return ENOMEM;
        r->data=p;
        v->dirty_bytes=v->dirty_bytes-r->capacity+cap; r->capacity=cap;
        if (!r->dirty && old) {
            e=fork_io(v,v->source,r->value+88,object_id(r),0,0,p,(size_t)old,false);
            if (e) return e;
        }
    }
    r->dirty=true; return 0;
}
static void changed(hfs_offline *v,record *r) { v->dirty=true; w32(r->value+16,now_mac()); w32(r->value+20,now_mac()); }
int hfs_offline_write(hfs_offline *v,uint32_t id,uint64_t offset,const void *bytes,size_t length) {
    if (!v || (!bytes && length) || v->failed) return EINVAL;
    record *r=by_id(v,id); if (!r) return ENOENT;
    if (!length) return 0;
    if (offset>MAX_DIRTY || length>MAX_DIRTY-offset) return EFBIG;
    uint64_t old=r64(r->value+88),end=offset+length;
    int e=buffer_file(v,r,end); if (e) return e;
    if (offset>old) memset(r->data+(size_t)old,0,(size_t)(offset-old));
    memcpy(r->data+(size_t)offset,bytes,length);
    if (end>old) w64(r->value+88,end);
    changed(v,r); return 0;
}
int hfs_offline_truncate(hfs_offline *v,uint32_t id,uint64_t length) {
    if (!v || v->failed) return EINVAL;
    record *r=by_id(v,id); if (!r) return ENOENT;
    uint64_t old=r16(r->value)==2?r64(r->value+88):0;
    int e=buffer_file(v,r,length); if (e) return e;
    if (length>old) memset(r->data+(size_t)old,0,(size_t)(length-old));
    w64(r->value+88,length); changed(v,r); return 0;
}
static int valid_parent(hfs_offline *v,uint32_t parent,const uint16_t *name,size_t length) {
    if (!v || v->failed || !name || !length || length>255) return EINVAL;
    if ((length==1 && name[0]=='.') || (length==2 && name[0]=='.' && name[1]=='.')) return EINVAL;
    for (size_t i=0;i<length;i++) if (!name[i] || name[i]=='/') return EINVAL;
    record *r=by_id(v,parent); if (!r || r->removed) return ENOENT;
    return r16(r->value)==1?0:ENOTDIR;
}
static uint8_t *make_key(uint32_t parent,const uint16_t *name,size_t length) {
    uint8_t *key=(uint8_t *)calloc(1,8+2*length); if (!key) return NULL;
    w16(key,(uint16_t)(6+2*length)); w32(key+2,parent); w16(key+6,(uint16_t)length);
    for (size_t i=0;i<length;i++) w16(key+8+2*i,name[i]);
    return key;
}
static record *thread_for(hfs_offline *v,uint32_t id) {
    for (size_t i=0;i<v->count;i++) {
        record *r=v->records[i];
        if (!r->removed && !object_id(r) && r32(r->key+2)==id && r16(r->key+6)==0) return r;
    }
    return NULL;
}
static void parent_changed(hfs_offline *v,uint32_t parent,int delta,bool folder) {
    record *r=by_id(v,parent); if (!r) return;
    w32(r->value+4,(uint32_t)((int64_t)r32(r->value+4)+delta));
    if (folder && (r16(r->value+2)&0x10)) w32(r->value+84,(uint32_t)((int64_t)r32(r->value+84)+delta));
    changed(v,r);
}
int hfs_offline_create(hfs_offline *v,uint32_t parent,const uint16_t *name,size_t length,uint32_t mode,uint32_t *id) {
    int e=valid_parent(v,parent,name,length); if (e) return e;
    if (!id || ((mode&TYPE_MODE)!=DIR_MODE && (mode&TYPE_MODE)!=FILE_MODE)) return EINVAL;
    hfs_offline_info found; if (!hfs_offline_lookup(v,parent,name,length,&found)) return EEXIST;
    if (v->next_id==UINT32_MAX || v->count>MAX_RECORDS-2) return ENOSPC;
    record *r=(record *)calloc(1,sizeof *r),*t=(record *)calloc(1,sizeof *t);
    if (!r || !t) { free(r); free(t); return ENOMEM; }
    bool folder=(mode&TYPE_MODE)==DIR_MODE;
    r->key=make_key(parent,name,length); r->key_length=(uint16_t)(8+2*length);
    r->value_length=folder?88:248; r->value=(uint8_t *)calloc(1,r->value_length);
    t->key=make_key(v->next_id,NULL,0); t->key_length=8;
    t->value_length=(uint16_t)(10+2*length); t->value=(uint8_t *)calloc(1,t->value_length);
    if (!r->key || !r->value || !t->key || !t->value) { free_record(r); free_record(t); return ENOMEM; }
    w16(r->value,folder?1:2); w16(r->value+2,folder?0x10:2); w32(r->value+8,v->next_id);
    for (unsigned i=12;i<=24;i+=4) w32(r->value+i,now_mac());
    w32(r->value+32,501); w32(r->value+36,501); w16(r->value+42,(uint16_t)mode);
    w16(t->value,folder?3:4); w32(t->value+4,parent); w16(t->value+8,(uint16_t)length);
    memcpy(t->value+10,r->key+8,2*length);
    e=add_record(v,r); if (e) { free_record(r); free_record(t); return e; }
    e=add_record(v,t); if (e) { v->count--; free_record(r); free_record(t); return e; }
    *id=v->next_id++; parent_changed(v,parent,1,folder); v->dirty=true; return 0;
}
int hfs_offline_remove(hfs_offline *v,uint32_t id) {
    if (!v || v->failed || id<16) return EINVAL;
    record *r=by_id(v,id); if (!r || r->removed) return ENOENT;
    for (size_t i=0;i<v->attribute_count;i++) {
        record *a=v->attributes[i];
        if (r32(a->key+4)==id && r32(a->value)!=0x10) return ENOTSUP;
    }
    bool folder=r16(r->value)==1;
    if (folder) {
        hfs_offline_info child; size_t cursor=0;
        if (!hfs_offline_next(v,id,&cursor,&child)) return ENOTEMPTY;
    } else {
        int e=fork_inline(v,r->value+168); if (e) return e;
        e=buffer_file(v,r,r64(r->value+88)); if (e) return e;
    }
    record *t=thread_for(v,id); if (!t) return EIO;
    for (size_t i=0;i<v->attribute_count;i++) if (r32(v->attributes[i]->key+4)==id) v->attributes[i]->removed=true;
    r->removed=t->removed=true; parent_changed(v,r32(r->key+2),-1,folder); v->dirty=true; return 0;
}
int hfs_offline_rename(hfs_offline *v,uint32_t id,uint32_t parent,const uint16_t *name,size_t length) {
    int e=valid_parent(v,parent,name,length); if (e) return e;
    record *r=by_id(v,id); if (!r || r->removed) return ENOENT;
    if (id<16) return EINVAL;
    bool folder=r16(r->value)==1;
    uint32_t walk=parent;
    for (size_t depth=0;walk!=2;depth++) {
        if (walk==id) return EINVAL;
        record *ancestor=by_id(v,walk);
        if (!ancestor || ancestor->removed || depth>=v->count) return EIO;
        walk=r32(ancestor->key+2);
    }
    hfs_offline_info dest;
    bool replace=!hfs_offline_lookup(v,parent,name,length,&dest);
    if (replace && dest.id==id) return 0;
    if (replace && ((dest.mode&TYPE_MODE)==DIR_MODE)!=folder) return folder?ENOTDIR:EISDIR;
    record *t=thread_for(v,id); if (!t) return EIO;
    uint8_t *key=make_key(parent,name,length),*value=(uint8_t *)calloc(1,10+2*length);
    if (!key || !value) { free(key); free(value); return ENOMEM; }
    if (replace && (e=hfs_offline_remove(v,dest.id))) { free(key); free(value); return e; }
    uint32_t old_parent=r32(r->key+2);
    free(r->key); r->key=key; r->key_length=(uint16_t)(8+2*length);
    w16(value,folder?3:4); w32(value+4,parent); w16(value+8,(uint16_t)length); memcpy(value+10,key+8,2*length);
    free(t->value); t->value=value; t->value_length=(uint16_t)(10+2*length);
    parent_changed(v,old_parent,-1,folder); parent_changed(v,parent,1,folder); changed(v,r); return 0;
}

/* Whole-tree construction. Every index separator is the first key in its
 * child. Siblings are linked at each height, and the bitmap covers ALL nodes.
 * A bounded catalog fits its node bitmap in the header's map record. */
typedef struct tree_node { uint8_t *bytes; record *first; uint32_t id; } tree_node;
typedef struct tree_build { tree_node *nodes; size_t count,capacity; uint16_t size; } tree_build;
static int tree_add(tree_build *b,uint8_t kind,uint8_t height,record *first,size_t *index) {
    if (b->count>=MAX_TREE/b->size) return EFBIG;
    if (b->count==b->capacity) {
        size_t n=b->capacity?b->capacity*2:64;
        tree_node *p=(tree_node *)realloc(b->nodes,n*sizeof(*p)); if (!p) return ENOMEM;
        b->nodes=p; b->capacity=n;
    }
    uint8_t *p=(uint8_t *)calloc(1,b->size); if (!p) return ENOMEM;
    p[8]=kind; p[9]=height; w16(p+b->size-2,14);
    *index=b->count; tree_node node={p,first,(uint32_t)b->count}; b->nodes[b->count++]=node; return 0;
}
static bool tree_put(tree_build *b,size_t index,const record *r,uint32_t child) {
    uint8_t *p=b->nodes[index].bytes;
    uint16_t count=r16(p+10),off=r16(p+b->size-2*(count+1));
    uint16_t length=(uint16_t)(r->key_length+(child?4:r->value_length));
    if ((uint32_t)off+length>b->size-2u*(count+2u)) return false;
    memcpy(p+off,r->key,r->key_length);
    if (child) w32(p+off+r->key_length,child); else memcpy(p+off+r->key_length,r->value,r->value_length);
    w16(p+10,(uint16_t)(count+1)); w16(p+b->size-2*(count+2),(uint16_t)(off+length)); return true;
}
static void tree_links(tree_build *b,size_t first,size_t end) {
    for (size_t i=first;i<end;i++) {
        w32(b->nodes[i].bytes,i+1<end?(uint32_t)(i+1):0);
        w32(b->nodes[i].bytes+4,i>first?(uint32_t)(i-1):0);
    }
}
static int tree_bytes(record **records,size_t record_count,const uint8_t *tree_header,uint16_t node_size,
                      bool catalog,uint8_t **out,size_t *length,uint32_t *files,uint32_t *folders) {
    record **sorted=(record **)malloc((record_count?record_count:1)*sizeof(*sorted)); if (!sorted) return ENOMEM;
    size_t n=0; *files=*folders=0;
    for (size_t i=0;i<record_count;i++) if (!records[i]->removed) {
        record *r=records[i]; sorted[n++]=r;
        if (catalog && r16(r->value)==2) (*files)++;
        else if (catalog && r16(r->value)==1 && object_id(r)!=2) (*folders)++;
    }
    if (catalog) qsort(sorted,n,sizeof(*sorted),key_compare);
    int e=0; tree_build b={0}; b.size=node_size;
    size_t current=0;
    e=tree_add(&b,1,0,NULL,&current); if (e) goto done;
    e=tree_add(&b,255,1,n?sorted[0]:NULL,&current); if (e) goto done;
    for (size_t i=0;i<n;i++) {
        if (catalog && i && key_compare(&sorted[i-1],&sorted[i])>=0) { e=EIO; goto done; }
        if (!tree_put(&b,current,sorted[i],0)) {
            e=tree_add(&b,255,1,sorted[i],&current); if (e) goto done;
            if (!tree_put(&b,current,sorted[i],0)) { e=EFBIG; goto done; }
        }
    }
    size_t first=1,end=b.count,last_leaf=end-1; uint8_t height=1;
    tree_links(&b,first,end);
    while (end-first>1) {
        if (++height>16) { e=EFBIG; goto done; }
        size_t next=b.count;
        e=tree_add(&b,0,height,b.nodes[first].first,&current); if (e) goto done;
        for (size_t i=first;i<end;i++) {
            record *key=b.nodes[i].first; uint32_t id=b.nodes[i].id;
            if (!key) { e=EIO; goto done; }
            if (!tree_put(&b,current,key,id)) {
                e=tree_add(&b,0,height,key,&current); if (e) goto done;
                if (!tree_put(&b,current,key,id)) { e=EFBIG; goto done; }
            }
        }
        first=next; end=b.count; tree_links(&b,first,end);
    }
    size_t map_start=248,map_end=b.size-8;
    size_t total=r32(tree_header+22);
    if (total<b.count) total=b.count;
    if (total>(map_end-map_start)*8 || total>MAX_TREE/b.size) { e=EFBIG; goto done; }
    uint8_t *head=b.nodes[0].bytes; memcpy(head+14,tree_header,106);
    w16(head+10,3); w16(head+b.size-2,14); w16(head+b.size-4,120);
    w16(head+b.size-6,(uint16_t)map_start); w16(head+b.size-8,(uint16_t)map_end);
    if (!n) { first=0; last_leaf=0; height=0; }
    w16(head+14,height); w32(head+16,(uint32_t)first); w32(head+20,(uint32_t)n);
    w32(head+24,n?1:0); w32(head+28,(uint32_t)last_leaf); w32(head+36,(uint32_t)total); w32(head+40,(uint32_t)(total-b.count));
    for (size_t i=0;i<b.count;i++) head[map_start+i/8]|=(uint8_t)(0x80u>>(i%8));
    *length=total*b.size; *out=(uint8_t *)calloc(1,*length);
    if (!*out) { e=ENOMEM; goto done; }
    for (size_t i=0;i<b.count;i++) memcpy(*out+i*b.size,b.nodes[i].bytes,b.size);
done:
    for (size_t i=0;i<b.count;i++) free(b.nodes[i].bytes);
    free(b.nodes); free(sorted); return e;
}
static bool used(const uint8_t *map,uint32_t block) { return (map[block/8]&(0x80u>>(block%8)))!=0; }
static void bit_set(uint8_t *map,uint32_t block,bool set) {
    uint8_t bit=(uint8_t)(0x80u>>(block%8));
    if (set) map[block/8]|=bit; else map[block/8]&=(uint8_t)~bit;
}
static int claim_extents(hfs_offline *v,const uint8_t *extents,const uint8_t *map,uint8_t *claimed,uint32_t *blocks) {
    for (unsigned i=0;i<8;i++) {
        uint32_t start=r32(extents+8*i),count=r32(extents+8*i+4);
        if (!count) { if (start) return EIO; continue; }
        if (!start || start>=v->block_count || count>v->block_count-start || count>UINT32_MAX-*blocks) return EIO;
        for (uint32_t b=start;b<start+count;b++) {
            if (!used(map,b) || used(claimed,b)) return EIO;
            bit_set(claimed,b,true);
        }
        *blocks+=count;
    }
    return 0;
}
static int claim_fork(hfs_offline *v,const uint8_t *fork,uint32_t id,uint8_t type,const uint8_t *map,uint8_t *claimed) {
    uint32_t blocks=0,total=r32(fork+12);
    int e=claim_extents(v,fork+16,map,claimed,&blocks); if (e) return e;
    size_t steps=0;
    while (blocks<total) {
        record *next=NULL;
        for (size_t i=0;i<v->extent_count;i++) {
            record *r=v->extents[i];
            if (r->key[2]==type && r32(r->key+4)==id && r32(r->key+8)==blocks) { next=r; break; }
        }
        if (!next || ++steps>v->extent_count) return EIO;
        uint32_t before=blocks;
        e=claim_extents(v,next->value,map,claimed,&blocks); if (e) return e;
        if (before==blocks) return EIO;
    }
    return blocks!=total || r64(fork)>(uint64_t)blocks*v->block_size?EIO:0;
}
static int audit_allocations(hfs_offline *v) {
    size_t length=(size_t)r64(v->header+112);
    uint8_t *map=(uint8_t *)malloc(length),*claimed=(uint8_t *)calloc(1,length);
    if (!map || !claimed) { free(map); free(claimed); return ENOMEM; }
    int e=fork_io(v,v->source,v->header+112,6,0,0,map,length,false); if (e) goto done;
    uint32_t first=(1536u+v->block_size-1)/v->block_size;
    uint32_t last=v->block_count-(1024u+v->block_size-1)/v->block_size;
    for (uint32_t b=0;b<first;b++) bit_set(claimed,b,true);
    for (uint32_t b=last;b<v->block_count;b++) bit_set(claimed,b,true);
    const unsigned offsets[5]={112,192,272,352,432};
    const uint32_t ids[5]={6,3,4,8,7};
    for (unsigned i=0;i<5;i++) { e=claim_fork(v,v->header+offsets[i],ids[i],0,map,claimed); if (e) goto done; }
    for (size_t i=0;i<v->count;i++) {
        record *r=v->records[i]; if (r16(r->value)!=2) continue;
        e=claim_fork(v,r->value+88,object_id(r),0,map,claimed); if (e) goto done;
        e=claim_fork(v,r->value+168,object_id(r),255,map,claimed); if (e) goto done;
    }
    for (size_t i=0;i<v->attribute_count;i++) {
        record *r=v->attributes[i]; uint32_t type=r32(r->value),blocks=0;
        if (type==0x20 || type==0x30) {
            e=claim_extents(v,r->value+(type==0x20?24:8),map,claimed,&blocks); if (e) goto done;
        }
    }
done: free(map); free(claimed); return e;
}
static int free_fork(hfs_offline *v,uint8_t *map,const uint8_t *fork) {
    int e=fork_inline(v,fork); if (e) return e;
    for (unsigned i=0;i<8;i++) {
        uint32_t start=r32(fork+16+i*8),count=r32(fork+20+i*8);
        for (uint32_t j=0;j<count;j++) {
            if (!used(map,start+j)) return EIO;
            bit_set(map,start+j,false);
        }
    }
    return 0;
}
static int allocate_fork(hfs_offline *v,uint8_t *map,uint64_t length,uint8_t fork[80]) {
    memset(fork,0,80); w64(fork,length);
    uint64_t need=(length+v->block_size-1)/v->block_size;
    if (need>UINT32_MAX) return EFBIG;
    w32(fork+12,(uint32_t)need);
    unsigned extent=0; uint32_t start=0,count=0;
    /* The header and alternate header allocation blocks are never data. */
    uint32_t first=(1536u+v->block_size-1)/v->block_size;
    uint32_t last=v->block_count-(1024u+v->block_size-1)/v->block_size;
    for (uint32_t b=first;b<last && need;b++) {
        if (used(map,b)) {
            if (count) { w32(fork+16+extent*8,start); w32(fork+20+extent*8,count); extent++; count=0; }
            if (extent==8) return ENOSPC;
            continue;
        }
        if (!count) start=b;
        count++; need--; bit_set(map,b,true);
    }
    if (count) { if (extent==8) return ENOSPC; w32(fork+16+extent*8,start); w32(fork+20+extent*8,count); }
    return need?ENOSPC:0;
}
int hfs_offline_commit(hfs_offline *v,const char *path) {
    if (!v || !path || v->failed) return EINVAL;
    if (!v->dirty) return 0;
    FILE *dest=fopen(path,"r+b"); if (!dest) return errno;
    int e=0; uint8_t *bitmap=NULL,*catalog=NULL,*attributes=NULL; size_t catalog_length=0,attributes_length=0;
    bool attributes_changed=false;
    for (size_t i=0;i<v->attribute_count;i++) if (v->attributes[i]->removed) attributes_changed=true;
    /* CRT st_ino is zero on Windows. Compare real file IDs there, including
     * hard links, rather than treating every file as an alias. */
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION source_stat,dest_stat;
    if (!GetFileInformationByHandle((HANDLE)_get_osfhandle(file_no(v->source)),&source_stat) ||
        !GetFileInformationByHandle((HANDLE)_get_osfhandle(file_no(dest)),&dest_stat)) { e=EIO; goto done; }
    if (source_stat.dwVolumeSerialNumber==dest_stat.dwVolumeSerialNumber &&
        source_stat.nFileIndexHigh==dest_stat.nFileIndexHigh && source_stat.nFileIndexLow==dest_stat.nFileIndexLow) { e=EINVAL; goto done; }
#else
    struct stat source_stat,dest_stat;
    if (fstat(file_no(v->source),&source_stat) || fstat(file_no(dest),&dest_stat)) { e=EIO; goto done; }
    if (source_stat.st_dev==dest_stat.st_dev && source_stat.st_ino==dest_stat.st_ino) { e=EINVAL; goto done; }
#endif
    if (seek_file(dest,0,SEEK_END) || (uint64_t)tell_file(dest)!=v->image_size) { e=EINVAL; goto done; }
    uint8_t check[512]; e=io_at(dest,v->image_size,1024,check,512,false); if (e) goto done;
    if (memcmp(check,v->header,512)) { e=EINVAL; goto done; }
    size_t bitmap_length=(size_t)r64(v->header+112);
    bitmap=(uint8_t *)malloc(bitmap_length); if (!bitmap) { e=ENOMEM; goto done; }
    e=fork_io(v,v->source,v->header+112,6,0,0,bitmap,bitmap_length,false); if (e) goto done;
    /* Free only old allocations in the private bitmap; the live image is
     * never written. All data of changed/unlinked files is already buffered. */
    e=free_fork(v,bitmap,v->header+272); if (e) goto done;
    if (attributes_changed) { e=free_fork(v,bitmap,v->header+352); if (e) goto done; }
    for (size_t i=0;i<v->count;i++) {
        record *r=v->records[i]; if (r16(r->value)!=2 || (!r->dirty && !r->removed)) continue;
        /* The logical size may already have grown; allocation ownership is
         * described by totalBlocks/extents, independently of the new EOF. */
        uint8_t old[80]; memcpy(old,r->value+88,80); w64(old,0);
        e=free_fork(v,bitmap,old); if (e) goto done;
        if (r->removed) { e=free_fork(v,bitmap,r->value+168); if (e) goto done; }
    }
    for (size_t i=0;i<v->count;i++) {
        record *r=v->records[i]; if (r16(r->value)!=2 || !r->dirty || r->removed) continue;
        uint8_t fork[80]; size_t length=(size_t)r64(r->value+88);
        e=allocate_fork(v,bitmap,length,fork); if (e) goto done;
        e=fork_io(v,dest,fork,object_id(r),0,0,r->data,length,true); if (e) goto done;
        memcpy(r->value+88,fork,80);
    }
    uint32_t files=0,folders=0;
    e=tree_bytes(v->records,v->count,v->tree_header,v->node_size,true,&catalog,&catalog_length,&files,&folders); if (e) goto done;
    uint8_t catalog_fork[80]; e=allocate_fork(v,bitmap,catalog_length,catalog_fork); if (e) goto done;
    e=fork_io(v,dest,catalog_fork,4,0,0,catalog,catalog_length,true); if (e) goto done;
    uint8_t attribute_fork[80]; memcpy(attribute_fork,v->header+352,80);
    if (attributes_changed) {
        uint32_t unused_files,unused_folders;
        e=tree_bytes(v->attributes,v->attribute_count,v->attribute_header,v->attribute_node_size,false,
            &attributes,&attributes_length,&unused_files,&unused_folders); if (e) goto done;
        e=allocate_fork(v,bitmap,attributes_length,attribute_fork); if (e) goto done;
        e=fork_io(v,dest,attribute_fork,8,0,0,attributes,attributes_length,true); if (e) goto done;
    }
    e=fork_io(v,dest,v->header+112,6,0,0,bitmap,bitmap_length,true); if (e) goto done;
    uint8_t header[512]; memcpy(header,v->header,512); memcpy(header+272,catalog_fork,80);
    memcpy(header+352,attribute_fork,80);
    uint32_t free_blocks=0; for (uint32_t b=0;b<v->block_count;b++) if (!used(bitmap,b)) free_blocks++;
    w32(header+32,files); w32(header+36,folders); w32(header+48,free_blocks);
    w32(header+64,v->next_id); w32(header+20,now_mac()); w32(header+68,r32(header+68)+1);
    w32(header+4,(r32(header+4)|0x100u)&~0x800u); /* clean, no inconsistent bit */
    e=io_at(dest,v->image_size,1024,header,512,true); if (e) goto done;
    e=io_at(dest,v->image_size,v->image_size-1024,header,512,true); if (e) goto done;
    if (fflush(dest) || sync_file(file_no(dest))) { e=errno?errno:EIO; goto done; }
    fclose(v->source); v->source=dest; dest=NULL; memcpy(v->header,header,512);
    for (size_t i=0;i<v->count;i++) {
        record *r=v->records[i];
        if (r->removed) {
            /* Its old blocks are gone. Keep open-unlinked bytes only in RAM. */
            if (r16(r->value)==2) { memset(r->value+100,0,68); memset(r->value+168,0,80); }
        } else if (r->dirty) {
            v->dirty_bytes-=r->capacity; free(r->data); r->data=NULL; r->capacity=0; r->dirty=false;
        }
    }
    v->dirty=false;
done:
    free(bitmap); free(catalog); free(attributes); if (dest) fclose(dest);
    if (e) v->failed=true; /* No retry after an incomplete unpublished write. */
    return e;
}
