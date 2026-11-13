/*
 * Windows host VFS model. Production functions are appended by the extractor.
 * The model enforces iocount ownership and walks a small in-memory tree.
 * It is NOT XNU namei/APFS/MAC/namecache or LaunchServices.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define NULLVP ((vnode_t)0)
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EACCES 13
#define EXDEV 18
#define ENOTDIR 20
#define EINVAL 22
#define EROFS 30
#define ELOOP 62
#define ENOTSUP 45
#define ESTALE 70
#define ENAMETOOLONG 63
#define NAME_MAX 255
#define MAXPATHLEN 1024
#define Z_WAITOK 1
#define Z_NOFAIL 2
typedef uint64_t ino64_t;
#define EJUSTRETURN (-2)
#define LOOKUP 0
#define CREATE 1
#define DELETE 2
#define RENAME 3
#define MAKEENTRY (1U<<0)
#define NOCROSSMOUNT (1U<<1)
#define LOCKPARENT (1U<<2)
#define WANTPARENT (1U<<3)
#define SAVESTART (1U<<4)
#define ISLASTCN (1U<<5)
#define CN_NBMOUNTLOOK (1U<<6)
#define CN_FIRMLINK_NOFOLLOW (1U<<7)
#define USEDVP (1U<<8)
#define NOFOLLOW (1U<<9)
#define NOFOLLOW_ANY (1U<<10)
#define LOCKLEAF (1U<<11)
#define CN_RAW_ENCRYPTED (1U<<12)
#define FOLLOW (1U<<13)
#define VNODE_LOOKUP_NOFOLLOW NOFOLLOW
#define VNODE_LOOKUP_NOFOLLOW_ANY NOFOLLOW_ANY
#define VNODE_LOOKUP_NOCROSSMOUNT NOCROSSMOUNT
#define NAMEI_ROOTDIR (1U<<0)
#define NAMEI_RESOLVE_BENEATH (1U<<1)
#define NAMEI_NODOTDOT (1U<<2)
#define NAMEI_NOUNION (1U<<3)
#define NAMEI_UNIQUE (1U<<4)
#define NAMEI_COMPOUNDOPEN (1U<<5)
#define NAMEI_NOFOLLOW_ANY (1U<<6)
#define NAMEI_LOCAL (1U<<7)
#define NAMEI_NODEVFS (1U<<8)
#define NAMEI_IMMOVABLE (1U<<9)
#define BINDM_FORCE_ROOT 0x80000000ULL
#define BINDM_WEBKIT_SYSTEMOS 0x40000000ULL
#define BINDM_WEBKIT_NATIVE 0x20000000ULL
#define BINDM_CASEINSENSITIVE 2
#define BIND_NODE_ROOT 1
#define BIND_NODE_NOCACHE 2
#define BIND_WEBKIT_BUNDLE_COUNT 5
#define BIND_ROOT_INO 2
#define MNT_RDONLY 1
#define MNT_NOSUID 2
#define MNT_NOEXEC 4
#define MNT_NODEV 8
#define MNT_QUARANTINE 16
#define MNT_LOCAL 32
#define MNT_REMOVABLE 64
#define MNTK_VIRTUALDEV 1
#define VA_REALFSID 0x0200000
#define VT_BINDFS 1
#define VT_DEVFS 2

typedef struct vnode *vnode_t;
typedef struct mount *mount_t;
typedef struct host_context *vfs_context_t;
typedef struct { int val[2]; } fsid_t;
struct vfsstatfs { fsid_t f_fsid; };
struct bind_mount {
    vnode_t bindm_lowerrootvp, bindm_rootvp;
    uint32_t bindm_lowerrootvid;
    uint64_t bindm_flags;
    uint64_t bindm_webkit_fileids[BIND_WEBKIT_BUNDLE_COUNT];
    uint64_t bindm_webkit_library_id, bindm_webkit_extensionkit_id;
    int bindm_webkit_error;
};
struct mount { struct bind_mount bind; struct vfsstatfs stat; uint64_t flags; unsigned mnt_kern_flag; };
enum vtype { VDIR, VLNK, VREG };
struct vnode {
    mount_t mp;
    vnode_t parent, lower;
    mount_t mountedhere;
    const char *name;
    uint64_t id;
    enum vtype type;
    bool root, omit_fileid;
    unsigned iocount, vid, tag;
};
struct host_context { unsigned identity; };
struct nameidata { vnode_t ni_rootdir; unsigned ni_flag; const char *ni_next; };
struct componentname {
    int cn_nameiop, cn_namelen;
    unsigned cn_flags;
    const char *cn_nameptr;
    struct nameidata *cn_ndp;
};
struct vnop_lookup_args { vnode_t a_dvp; vnode_t *a_vpp; struct componentname *a_cnp; vfs_context_t a_context; };
struct vnode_attr {
    uint64_t va_active, va_supported, va_vaflags;
    uint64_t va_fileid, va_linkid, va_parentid, va_fsid, va_uid, va_gid;
    fsid_t va_fsid64;
};
struct vnop_getattr_args { vnode_t a_vp; struct vnode_attr *a_vap; vfs_context_t a_context; };
enum { va_fileid=1, va_linkid=2, va_parentid=4, va_fsid=8, va_fsid64=16, va_uid=32, va_gid=64 };
#define VATTR_INIT(a) ((a)->va_active=(a)->va_supported=(a)->va_vaflags=0)
#define VATTR_WANTED(a,b) ((a)->va_active |= (b))
#define VATTR_IS_ACTIVE(a,b) ((a)->va_active & (b))
#define VATTR_IS_SUPPORTED(a,b) ((a)->va_supported & (b))
#define VATTR_ALL_SUPPORTED(a) (((a)->va_active & (a)->va_supported) == (a)->va_active)
#define VATTR_RETURN(a,b,v) do {(a)->b=(v); (a)->va_supported|=(b);} while(0)
#define BINDFSDEBUG(...) ((void)0)
#define printf host_printf
static void host_printf(const char *format, ...) {(void)format;}
#define MOUNTTOBINDMOUNT(m) (&(m)->bind)
#define BINDVPTOLOWERVP(v) ((v)->lower)
#define VNOP_LOOKUP(d,v,c,x) lower_lookup(d,v,c,x)
#define VNOP_GETATTR(v,a,c) vnode_getattr(v,a,c)
#define VFS_VGET(m,id,v,c) host_vget(m,id,v,c)
#define __unused

__declspec(dllimport) void *GetStdHandle(unsigned int);
__declspec(dllimport) int WriteFile(void *,const void *,unsigned int,unsigned int *,void *);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(unsigned int);
static size_t strlen(const char *s) {size_t n=0;while(s[n])++n;return n;}
static int memcmp(const void *x,const void *y,size_t n) {const unsigned char *a=x,*b=y;for(size_t i=0;i<n;++i)if(a[i]!=b[i])return a[i]-b[i];return 0;}
static int strncmp(const char *a,const char *b,size_t n) {for(size_t i=0;i<n;++i){if(a[i]!=b[i])return (unsigned char)a[i]-(unsigned char)b[i];if(!a[i])break;}return 0;}
static int strcmp(const char *a,const char *b) {return strncmp(a,b,strlen(a)+1);}
static void *memset(void *p,int c,size_t n) {unsigned char *s=p;for(size_t i=0;i<n;++i)s[i]=(unsigned char)c;return p;}
static void *memcpy(void *p,const void *q,size_t n) {unsigned char *a=p;const unsigned char *b=q;for(size_t i=0;i<n;++i)a[i]=b[i];return p;}
static void say(const char *s) {unsigned int n;WriteFile(GetStdHandle((unsigned int)-11),s,(unsigned int)strlen(s),&n,NULL);}
static void say_uint(unsigned n) {char a[16];unsigned i=0;do{a[i++]=(char)('0'+n%10);n/=10;}while(n);while(i){char x[2]={a[--i],0};say(x);}}
static unsigned assertions, cases;
static const char *current_case="setup";
static void fail(const char *file,unsigned line,const char *expression) {say("FAIL ");say(current_case);say(" ");say(file);say(":");say_uint(line);say(" ");say(expression);say("\n");ExitProcess(1);}
#define CHECK(x) do {++assertions;if(!(x))fail(__FILE__,__LINE__,#x);} while(0)

enum { ROOT, SYSTEM, LIBRARY, EXTENSIONKIT, EXTENSIONS, FRAMEWORKS, WEB0, WEB1, WEB2, WEB3, WEB4, INFO, OTHER_CHILD, NODE_COUNT };
static struct mount native_mount, alias_mount, foreign_mount;
static struct vnode nodes[NODE_COUNT], aliases[NODE_COUNT];
static vnode_t rootvnode=&nodes[ROOT];
static struct host_context caller={0x1234}, foreign_caller={0x5678};
static vfs_context_t expected_context=&caller;
static unsigned name_refs, lookup_calls, inner_calls, lower_calls, nodeget_calls, vget_calls, getattr_calls;
static unsigned last_nodeget_flags, last_inner_flags;
static bool boot_arg_present=true, in_prepare, strict_start_ref=true;
static uint32_t native_view_flag=1;
static int getref_fail_node=-1, lookup_fail_node=-1, getattr_fail_node=-1, vget_fail_node=-1, wrong_reverse_node=-1;
static int lookup_errno=EACCES, inner_errno, lower_errno, nodeget_errno;
static char alloc_buffers[2][MAXPATHLEN];
static bool alloc_used[2];
static int getpath_fail_node=-1;
static bool alias_root_getref_fail;
static bool bad_canonical_parent, bad_canonical_name, omit_canonical_name, omit_ndp;
static unsigned request_flags, request_namei_flags;
static vnode_t request_root;
static int node_index(vnode_t v) {for(int i=0;i<NODE_COUNT;++i)if(v==&nodes[i])return i;return -1;}
static bool is_alias(vnode_t v) {return v->mp==&alias_mount;}
static int ascii_lower(int c) {return c>='A'&&c<='Z'?c+32:c;}
static bool name_equal(const char *a,size_t alen,const char *b) {if(alen!=strlen(b))return false;for(size_t i=0;i<alen;++i)if(ascii_lower((unsigned char)a[i])!=ascii_lower((unsigned char)b[i]))return false;return true;}
static void require_context(vfs_context_t c) {CHECK(c==expected_context);}
static void require_prepare_root(void) {if(in_prepare)CHECK(nodes[SYSTEM].iocount>0);}
static int vnode_get(vnode_t v) {CHECK(v!=NULL);++v->iocount;return 0;}
static int vnode_getwithref(vnode_t v) {CHECK(v!=NULL);if((getref_fail_node>=0 && node_index(v)==getref_fail_node) || (alias_root_getref_fail && v==&aliases[SYSTEM]))return EIO;return vnode_get(v);}
static void vnode_put(vnode_t v) {CHECK(v!=NULL && v->iocount>0);--v->iocount;}
static mount_t vnode_mount(vnode_t v) {CHECK(v!=NULL);return v->mp;}
static vnode_t vnode_parent(vnode_t v) {return v->parent;}
static bool vnode_isdir(vnode_t v) {return v && v->type==VDIR;}
static bool vnode_isvroot(vnode_t v) {return v && v->root;}
static enum vtype vnode_vtype(vnode_t v) {return v->type;}
static mount_t vnode_mountedhere(vnode_t v) {return v->mountedhere;}
static int vnode_tag(vnode_t v) {return is_alias(v)?VT_BINDFS:(int)v->tag;}
static unsigned vnode_vid(vnode_t v) {return v->vid;}
static struct vfsstatfs *vfs_statfs(mount_t m) {return &m->stat;}
static uint64_t vfs_flags(mount_t m) {return m->flags;}
static void vnode_getparent_and_name(vnode_t v,vnode_t *p,const char **n) {
    CHECK(v->iocount>0);
    *p=bad_canonical_parent?&nodes[FRAMEWORKS]:v->parent;
    if(*p)vnode_get(*p);
    *n=omit_canonical_name?NULL:(bad_canonical_name?"wrong":v->name);
    if(*n)++name_refs;
}
static void vnode_putname(const char *n) {CHECK(n && name_refs);--name_refs;}
static bool PE_parse_boot_argn(const char *arg,uint32_t *out,unsigned size) {
    CHECK(!strcmp(arg,"webkit-native-view") && size==sizeof(uint32_t));*out=native_view_flag;return boot_arg_present;
}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx) {
    require_context(ctx);require_prepare_root();CHECK(v->iocount>0);++getattr_calls;
    if(node_index(v)==getattr_fail_node)return EIO;
    if(!v->omit_fileid && VATTR_IS_ACTIVE(a,va_fileid))VATTR_RETURN(a,va_fileid,v->id);
    if(VATTR_IS_ACTIVE(a,va_linkid))VATTR_RETURN(a,va_linkid,v->id);
    if(VATTR_IS_ACTIVE(a,va_parentid))VATTR_RETURN(a,va_parentid,v->parent?v->parent->id:v->id);
    if(VATTR_IS_ACTIVE(a,va_fsid))VATTR_RETURN(a,va_fsid,v->mp->stat.f_fsid.val[0]);
    if(VATTR_IS_ACTIVE(a,va_fsid64))VATTR_RETURN(a,va_fsid64,v->mp->stat.f_fsid);
    if(VATTR_IS_ACTIVE(a,va_uid))VATTR_RETURN(a,va_uid,0);
    if(VATTR_IS_ACTIVE(a,va_gid))VATTR_RETURN(a,va_gid,0);
    return 0;
}
static int host_vget(mount_t mp,uint64_t id,vnode_t *out,vfs_context_t ctx) {
    require_context(ctx);require_prepare_root();++vget_calls;*out=NULL;
    for(int i=0;i<NODE_COUNT;++i)if(nodes[i].mp==mp && nodes[i].id==id) {
        if(i==vget_fail_node)return EIO;
        *out=i==wrong_reverse_node?&nodes[OTHER_CHILD]:&nodes[i];return vnode_get(*out);
    }
    return ENOENT;
}
static int native_child(vnode_t dir,const char *name,size_t len,vnode_t *out,vfs_context_t ctx,unsigned flags) {
    require_context(ctx);require_prepare_root();CHECK(dir && dir->iocount>0);*out=NULL;
    if(dir->type!=VDIR)return ENOTDIR;
    vnode_t found=NULL;
    if(len==1 && name[0]=='.')found=dir;
    else if(len==2 && name[0]=='.' && name[1]=='.')found=dir->parent?dir->parent:dir;
    else for(int i=0;i<NODE_COUNT;++i)if(nodes[i].parent==dir && name_equal(name,len,nodes[i].name)){found=&nodes[i];break;}
    if(!found)return ENOENT;
    if(node_index(found)==lookup_fail_node)return lookup_errno;
    if(found->type==VLNK && (flags&(NOFOLLOW|NOFOLLOW_ANY)))return ELOOP;
    if((found->mp!=dir->mp || found->mountedhere) && (flags&NOCROSSMOUNT))return EXDEV;
    *out=found;return vnode_get(found);
}
static int bind_lookup(struct vnop_lookup_args *);
static int path_walk(vnode_t start,const char *path,unsigned flags,unsigned ndflags,vnode_t ndroot,vnode_t *out,vfs_context_t ctx);
static int vnode_lookupat(const char *path,int flags,vnode_t *out,vfs_context_t ctx,vnode_t start) {
    require_context(ctx);require_prepare_root();CHECK(start && (!strict_start_ref || start->iocount>0));++lookup_calls;
    if(!is_alias(start) && start==&nodes[LIBRARY] && name_equal(path,strlen(path),"ExtensionKit")) {
        ++inner_calls;last_inner_flags=(unsigned)flags;
        if(inner_errno)return inner_errno;
    }
    return path_walk(start,path,(unsigned)flags,0,rootvnode,out,ctx);
}
static int lower_lookup(vnode_t dir,vnode_t *out,struct componentname *cnp,vfs_context_t ctx) {
    ++lower_calls;if(lower_errno)return lower_errno;
    return native_child(dir,cnp->cn_nameptr,(size_t)cnp->cn_namelen,out,ctx,0);
}
static int bind_nodeget(mount_t mp,vnode_t low,vnode_t parent,vnode_t *out,struct componentname *cnp,int flags) {
    CHECK(mp==&alias_mount && low && low->iocount>0);++nodeget_calls;last_nodeget_flags=(unsigned)flags;
    if(nodeget_errno)return nodeget_errno;
    int index=node_index(low);CHECK(index>=0);
    aliases[index].parent=parent;
    *out=&aliases[index];return vnode_get(*out);
}
static int path_walk(vnode_t start,const char *path,unsigned flags,unsigned ndflags,vnode_t ndroot,vnode_t *out,vfs_context_t ctx) {
    require_context(ctx);*out=NULL;vnode_t current=start;vnode_get(current);
    const char *p=path;while(*p=='/')++p;
    while(*p) {
        const char *end=p;while(*end && *end!='/')++end;
        const char *after=end;while(*after=='/')++after;
        struct nameidata nd={ndroot,ndflags,end};
        struct componentname c={LOOKUP,(int)(end-p),flags|MAKEENTRY|(!*after?ISLASTCN:0),p,omit_ndp?NULL:&nd};
        vnode_t next=NULL;int error;
        if(is_alias(current)) {
            struct vnop_lookup_args ap={current,&next,&c,ctx};
            error=bind_lookup(&ap);
        } else error=native_child(current,p,(size_t)(end-p),&next,ctx,flags);
        vnode_put(current);
        if(error){CHECK(next==NULL);return error;}
        CHECK(next && next->iocount>0);current=next;p=after;
    }
    *out=current;return 0;
}

static void *kalloc_data(size_t size,int flags) {
    CHECK(size==MAXPATHLEN && flags==(Z_WAITOK|Z_NOFAIL));
    for(unsigned i=0;i<2;++i)if(!alloc_used[i]){alloc_used[i]=true;return alloc_buffers[i];}
    CHECK(false);return NULL;
}
static void kfree_data(void *buffer,size_t size) {
    CHECK(size==MAXPATHLEN);
    for(unsigned i=0;i<2;++i)if(buffer==alloc_buffers[i]){CHECK(alloc_used[i]);alloc_used[i]=false;return;}
    CHECK(false);
}
static int vn_getpath(vnode_t vp,char *buffer,int *length) {
    CHECK(vp && vp->iocount>0);
    if(node_index(vp)==getpath_fail_node)return EIO;
    vnode_t chain[NODE_COUNT];unsigned count=0;vnode_t at=vp;
    while(at && at->parent){CHECK(count<NODE_COUNT);chain[count++]=at;at=at->parent;}
    unsigned used=0;buffer[used++]='/';
    while(count){const char *name=chain[--count]->name;size_t size=strlen(name);CHECK(used+size+1<(unsigned)*length);memcpy(buffer+used,name,size);used+=(unsigned)size;if(count)buffer[used++]='/';}
    buffer[used]=0;*length=(int)used+1;return 0;
}

static void assert_balanced(void) {for(int i=0;i<NODE_COUNT;++i){CHECK(nodes[i].iocount==0);CHECK(aliases[i].iocount==0);}CHECK(name_refs==0);CHECK(!alloc_used[0]&&!alloc_used[1]);}
static void reset_model(const char *name) {
    assert_balanced();current_case=name;++cases;
    memset(nodes,0,sizeof(nodes));memset(aliases,0,sizeof(aliases));memset(&native_mount,0,sizeof(native_mount));memset(&alias_mount,0,sizeof(alias_mount));memset(&foreign_mount,0,sizeof(foreign_mount));
    native_mount.flags=alias_mount.flags=foreign_mount.flags=MNT_RDONLY;
    native_mount.stat.f_fsid=(fsid_t){{501,21}};alias_mount.stat.f_fsid=(fsid_t){{703,57}};foreign_mount.stat.f_fsid=(fsid_t){{901,18}};
    const char *names[NODE_COUNT]={"","System","Library","ExtensionKit","Extensions","Frameworks","WebContentExtension.appex","WebContentEnhancedSecurityExtension.appex","WebContentCaptivePortalExtension.appex","GPUExtension.appex","NetworkingExtension.appex","Info.plist","Other"};
    const int parents[NODE_COUNT]={-1,ROOT,SYSTEM,LIBRARY,EXTENSIONKIT,LIBRARY,EXTENSIONS,EXTENSIONS,EXTENSIONS,EXTENSIONS,EXTENSIONS,WEB0,LIBRARY};
    for(int i=0;i<NODE_COUNT;++i){
        nodes[i]=(struct vnode){.mp=&native_mount,.parent=parents[i]<0?NULL:&nodes[parents[i]],.name=names[i],.id=(uint64_t)(100+i),.type=i==INFO?VREG:VDIR,.root=i==ROOT,.vid=(unsigned)i+1};
        aliases[i]=(struct vnode){.mp=&alias_mount,.parent=parents[i]<0?NULL:&aliases[parents[i]],.lower=&nodes[i],.name=names[i],.id=(uint64_t)(100+i),.type=nodes[i].type,.root=i==SYSTEM,.vid=(unsigned)i+1};
    }
    alias_mount.bind.bindm_lowerrootvp=&nodes[SYSTEM];alias_mount.bind.bindm_rootvp=&aliases[SYSTEM];
    alias_mount.bind.bindm_flags=BINDM_CASEINSENSITIVE;
    boot_arg_present=true;native_view_flag=1;in_prepare=false;strict_start_ref=true;expected_context=&caller;
    getref_fail_node=lookup_fail_node=getattr_fail_node=vget_fail_node=wrong_reverse_node=-1;
    lookup_errno=EACCES;inner_errno=lower_errno=nodeget_errno=0;getpath_fail_node=-1;alias_root_getref_fail=false;
    bad_canonical_parent=bad_canonical_name=omit_canonical_name=omit_ndp=false;request_flags=request_namei_flags=0;request_root=rootvnode;
    lookup_calls=inner_calls=lower_calls=nodeget_calls=vget_calls=getattr_calls=0;last_nodeget_flags=last_inner_flags=0;
}
