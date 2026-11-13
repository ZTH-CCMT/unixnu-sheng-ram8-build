/* Assertions exercise extracted code against host contracts, never a hand copy. */
static int prepare(void) {
    in_prepare=true;
    int error=bindfs_prepare_webkit_identity(&alias_mount.bind,&alias_mount,&nodes[SYSTEM],&caller);
    in_prepare=false;alias_mount.bind.bindm_webkit_error=error;return error;
}
static int selfcheck(void);
static void ready(const char *name) {
    reset_model(name);CHECK(prepare()==0);
    CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));
    CHECK(alias_mount.bind.bindm_webkit_library_id==nodes[LIBRARY].id);
    CHECK(alias_mount.bind.bindm_webkit_extensionkit_id==nodes[EXTENSIONKIT].id);
    CHECK(selfcheck()==0);CHECK(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE);
    assert_balanced();
}
static int lookup(vnode_t start,const char *path,vnode_t *result) {
    return path_walk(start,path,request_flags,request_namei_flags,request_root,result,&caller);
}
static void expect_path(vnode_t start,const char *path,vnode_t expected) {
    vnode_t got=NULL;CHECK(lookup(start,path,&got)==0);CHECK(got==expected);vnode_put(got);assert_balanced();
}
static void expect_error(vnode_t start,const char *path,int expected) {
    vnode_t got=NULL;int error=lookup(start,path,&got);
    if(error!=expected){say("actual errno=");say_uint((unsigned)error);say(" expected=");say_uint((unsigned)expected);say("\n");}
    CHECK(error==expected);CHECK(!got);assert_balanced();
}
static int selfcheck(void) {
    vnode_get(&aliases[SYSTEM]);int error=bindfs_webkit_identity_check(&aliases[SYSTEM],&caller);vnode_put(&aliases[SYSTEM]);assert_balanced();return error;
}
static void prepare_cases(void) {
    reset_model("prepare holds root through all lookups/getattrs/VGET");CHECK(prepare()==0);CHECK(vget_calls==5);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));assert_balanced();
    reset_model("prepare alone leaves view disabled until successful selfcheck");CHECK(prepare()==0);expect_path(&aliases[LIBRARY],"ExtensionKit",&aliases[EXTENSIONKIT]);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));CHECK(selfcheck()==0);expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);
    reset_model("prepare boot argument absent");boot_arg_present=false;CHECK(prepare()==ENOTSUP);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));assert_balanced();
    reset_model("prepare boot argument zero");native_view_flag=0;CHECK(prepare()==ENOTSUP);assert_balanced();
    reset_model("prepare lower root getwithref error");getref_fail_node=SYSTEM;CHECK(prepare()==EIO);assert_balanced();
    reset_model("prepare requires read-only native mount");native_mount.flags=0;CHECK(prepare()==EPERM);assert_balanced();
    const unsigned restrictions[]={MNT_NOSUID,MNT_NOEXEC,MNT_NODEV,MNT_QUARANTINE};
    for(unsigned i=0;i<4;++i){
        reset_model("prepare cannot escape alias mount restriction");alias_mount.flags|=restrictions[i];CHECK(prepare()==EPERM);assert_balanced();
        reset_model("prepare matching restriction accepted");alias_mount.flags|=restrictions[i];native_mount.flags|=restrictions[i];CHECK(prepare()==0);assert_balanced();
    }
    const int targets[]={LIBRARY,EXTENSIONKIT,EXTENSIONS,WEB0,WEB1,WEB2,WEB3,WEB4};
    for(unsigned i=0;i<8;++i){
        reset_model("prepare lookup permission/error cleanup");lookup_fail_node=targets[i];CHECK(prepare()==EACCES);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));assert_balanced();
    }
    for(int i=WEB0;i<=WEB4;++i){
        reset_model("prepare VGET failure cleanup");vget_fail_node=i;CHECK(prepare()==EIO);assert_balanced();
        reset_model("prepare VGET wrong-object rejection");wrong_reverse_node=i;CHECK(prepare()==EINVAL);assert_balanced();
        reset_model("prepare bundle missing fileid");nodes[i].omit_fileid=true;CHECK(prepare()==EINVAL);assert_balanced();
        reset_model("prepare bundle getattr error");getattr_fail_node=i;CHECK(prepare()==EINVAL);assert_balanced();
    }
    reset_model("prepare Library missing fileid");nodes[LIBRARY].omit_fileid=true;CHECK(prepare()==EINVAL);assert_balanced();
    reset_model("prepare ExtensionKit missing fileid");nodes[EXTENSIONKIT].omit_fileid=true;CHECK(prepare()==EINVAL);assert_balanced();
    reset_model("prepare Extensions not directory");nodes[EXTENSIONS].type=VREG;CHECK(prepare()==EINVAL);assert_balanced();
    reset_model("prepare symlink cannot certify");nodes[EXTENSIONKIT].type=VLNK;CHECK(prepare()==ELOOP);assert_balanced();
    reset_model("prepare child mount cannot certify");nodes[EXTENSIONKIT].mountedhere=&foreign_mount;CHECK(prepare()==EXDEV);assert_balanced();
    reset_model("prepare foreign bundle mount cannot certify");nodes[WEB3].mp=&foreign_mount;CHECK(prepare()==EXDEV);assert_balanced();
}
static void ordinary_cases(void) {
    const char *paths[]={"Library/ExtensionKit","Library/ExtensionKit/Extensions","Library/ExtensionKit/./Extensions","Library/./ExtensionKit/Extensions","library/extensionkit/extensions","Library/ExtensionKit/Extensions/","Library/ExtensionKit/Extensions/WebContentExtension.appex"};
    const int expected[]={EXTENSIONKIT,EXTENSIONS,EXTENSIONS,EXTENSIONS,EXTENSIONS,EXTENSIONS,WEB0};
    for(unsigned i=0;i<7;++i){ready("ordinary full path and spelling invariance");expect_path(&aliases[SYSTEM],paths[i],&nodes[expected[i]]);}
    ready("modeled absolute System mount dispatch");
    /* This maps an absolute namespace /System mount to its alias root. It does
       not claim to emulate XNU's absolute-path namei or mount traversal. */
    const char *absolute="/System/Library/ExtensionKit/Extensions";
    CHECK(!strncmp(absolute,"/System/",8));expect_path(&aliases[SYSTEM],absolute+8,&nodes[EXTENSIONS]);
    ready("relative directory-fd style last component");
    vnode_t library=NULL,kit=NULL,extensions=NULL;
    CHECK(lookup(&aliases[SYSTEM],"Library",&library)==0);CHECK(library==&aliases[LIBRARY]);
    request_flags=USEDVP;CHECK(lookup(library,"ExtensionKit",&kit)==0);CHECK(kit==&nodes[EXTENSIONKIT]);
    CHECK(lookup(kit,"./Extensions",&extensions)==0);CHECK(extensions==&nodes[EXTENSIONS]);
    vnode_put(extensions);vnode_put(kit);vnode_put(library);assert_balanced();
    const unsigned allowed[]={ISLASTCN,USEDVP,NOFOLLOW,NOFOLLOW_ANY,CN_NBMOUNTLOOK,USEDVP|ISLASTCN|NOFOLLOW};
    for(unsigned i=0;i<6;++i){ready("allowed component flags preserve native identity");request_flags=allowed[i];expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);}
    const unsigned allowed_nd[]={NAMEI_NODOTDOT,NAMEI_NOUNION,NAMEI_UNIQUE,NAMEI_COMPOUNDOPEN,NAMEI_NOFOLLOW_ANY};
    for(unsigned i=0;i<5;++i){ready("non-root namei constraints preserve native identity");request_namei_flags=allowed_nd[i];expect_path(&aliases[LIBRARY],"ExtensionKit/Extensions",&nodes[EXTENSIONS]);}
    ready("lowerroot VROOT normal lookup is not an identity error");nodes[SYSTEM].root=true;unsigned before_nodes=nodeget_calls;expect_path(&aliases[SYSTEM],"Library",&aliases[LIBRARY]);CHECK(nodeget_calls==before_nodes+1);
    ready("unrelated Frameworks stays on alias volume");expect_path(&aliases[LIBRARY],"Frameworks",&aliases[FRAMEWORKS]);
    ready("native parent chain remains canonical");expect_path(&aliases[LIBRARY],"ExtensionKit/..",&nodes[LIBRARY]);expect_path(&aliases[LIBRARY],"ExtensionKit/../Frameworks",&nodes[FRAMEWORKS]);
    ready("five native bundles reverse-lookup pointer identity");
    for(int i=WEB0;i<=WEB4;++i){vnode_t got=NULL,reverse=NULL;CHECK(lookup(&aliases[LIBRARY],"ExtensionKit/Extensions",&got)==0);vnode_t bundle=NULL;CHECK(lookup(got,nodes[i].name,&bundle)==0);CHECK(bundle==&nodes[i]);CHECK(VFS_VGET(vnode_mount(bundle),bundle->id,&reverse,&caller)==0);CHECK(bundle==reverse);vnode_put(reverse);vnode_put(bundle);vnode_put(got);}assert_balanced();
    ready("same caller context reaches lower and inner authorization");expected_context=&foreign_caller;
    vnode_t got=NULL;CHECK(path_walk(&aliases[LIBRARY],"ExtensionKit",0,0,rootvnode,&got,&foreign_caller)==0);CHECK(got==&nodes[EXTENSIONKIT]);vnode_put(got);assert_balanced();
}
static void restriction_and_error_cases(void) {
    const unsigned denied[]={CN_FIRMLINK_NOFOLLOW,CN_RAW_ENCRYPTED};
    for(unsigned i=0;i<2;++i){ready("unsupported traversal mode fails explicitly");request_flags=denied[i];expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);}
    const unsigned denied_nd[]={NAMEI_ROOTDIR,NAMEI_RESOLVE_BENEATH};
    for(unsigned i=0;i<2;++i){ready("root and beneath constraints fail explicitly");request_namei_flags=denied_nd[i];expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);}
    ready("chroot boundary fails explicitly");request_root=&nodes[SYSTEM];expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);
    ready("missing nameidata fails explicitly");omit_ndp=true;expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);
    ready("lower permission failure retained");lower_errno=EACCES;expect_error(&aliases[LIBRARY],"ExtensionKit",EACCES);
    const int errors[]={EACCES,EIO,ENOENT,ELOOP,EXDEV};
    for(unsigned i=0;i<5;++i){ready("inner authorization errors never fallback");unsigned previous=nodeget_calls;inner_errno=errors[i];expect_error(&aliases[LIBRARY],"ExtensionKit",errors[i]);CHECK(nodeget_calls==previous);}
    ready("canonical parent mismatch rejected");bad_canonical_parent=true;expect_error(&aliases[LIBRARY],"ExtensionKit",EINVAL);
    ready("canonical name mismatch rejected");bad_canonical_name=true;expect_error(&aliases[LIBRARY],"ExtensionKit",EINVAL);
    ready("null canonical name rejected");omit_canonical_name=true;expect_error(&aliases[LIBRARY],"ExtensionKit",EINVAL);
    ready("runtime Library identity failure is explicit");getattr_fail_node=LIBRARY;expect_error(&aliases[LIBRARY],"ExtensionKit",EIO);
    ready("runtime child identity failure is explicit");getattr_fail_node=EXTENSIONKIT;expect_error(&aliases[LIBRARY],"ExtensionKit",EIO);
    ready("LOCAL rejects nonlocal target");request_namei_flags=NAMEI_LOCAL;expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);
    ready("LOCAL accepts local target");request_namei_flags=NAMEI_LOCAL;native_mount.flags|=MNT_LOCAL;expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);
    ready("NODEVFS rejects devfs target");request_namei_flags=NAMEI_NODEVFS;nodes[EXTENSIONKIT].tag=VT_DEVFS;expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);
    ready("NODEVFS accepts ordinary target");request_namei_flags=NAMEI_NODEVFS;expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);
    ready("IMMOVABLE rejects removable target");request_namei_flags=NAMEI_IMMOVABLE;native_mount.flags|=MNT_REMOVABLE;expect_error(&aliases[LIBRARY],"ExtensionKit",EXDEV);
    ready("IMMOVABLE permits virtual removable target");request_namei_flags=NAMEI_IMMOVABLE;native_mount.flags|=MNT_REMOVABLE;native_mount.mnt_kern_flag|=MNTK_VIRTUALDEV;expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);
    ready("lower ref acquisition failure");getref_fail_node=LIBRARY;expect_error(&aliases[LIBRARY],"ExtensionKit",EIO);
    ready("ordinary wrapper allocation error preserved");nodeget_errno=ENOMEM;expect_error(&aliases[LIBRARY],"Frameworks",ENOMEM);
    for(int operation=CREATE;operation<=RENAME;++operation){
        ready("readonly create/delete/rename preserved");vnode_t out=NULL;struct nameidata n={rootvnode,0,""};struct componentname c={operation,12,ISLASTCN,"ExtensionKit",&n};struct vnop_lookup_args a={&aliases[LIBRARY],&out,&c,&caller};CHECK(bind_lookup(&a)==EROFS);CHECK(!out);assert_balanced();
    }
}
static void covered_view_cases(void) {
    ready("bulk-style NOCROSS last entry remains enumerable");
    request_flags=USEDVP|NOCROSSMOUNT|ISLASTCN;
    expect_path(&aliases[LIBRARY],"ExtensionKit",&aliases[EXTENSIONKIT]);
    CHECK(last_nodeget_flags&BIND_NODE_NOCACHE);
    request_flags=0;expect_path(&aliases[LIBRARY],"ExtensionKit",&nodes[EXTENSIONKIT]);
    ready("covered view full path remains alias and ordinary path remains native");
    request_flags=NOCROSSMOUNT;expect_path(&aliases[SYSTEM],"Library/ExtensionKit/Extensions",&aliases[EXTENSIONS]);
    request_flags=0;expect_path(&aliases[SYSTEM],"Library/ExtensionKit/Extensions",&nodes[EXTENSIONS]);
}
static void reverse_cases(void) {
    ready("alias FSID plus root inode 2 reverses to alias root");
    vnode_t root_result=NULL;CHECK(bindfs_vget(&alias_mount,BIND_ROOT_INO,&root_result,&caller)==0);CHECK(root_result==&aliases[SYSTEM]);vnode_put(root_result);assert_balanced();
    ready("Library parentid 2 reverses to alias root");
    struct vnode_attr parent_attr;VATTR_INIT(&parent_attr);VATTR_WANTED(&parent_attr,va_parentid);struct vnop_getattr_args parent_args={&aliases[LIBRARY],&parent_attr,&caller};
    CHECK(bindfs_getattr(&parent_args)==0);CHECK(parent_attr.va_parentid==BIND_ROOT_INO);
    CHECK(bindfs_vget(&alias_mount,parent_attr.va_parentid,&root_result,&caller)==0);CHECK(root_result==&aliases[SYSTEM]);vnode_put(root_result);assert_balanced();
    ready("alias root inode acquisition error preserves reference balance");alias_root_getref_fail=true;
    CHECK(bindfs_vget(&alias_mount,BIND_ROOT_INO,&root_result,&caller)==EIO);CHECK(!root_result);assert_balanced();

    for(int target=SYSTEM;target<=WEB4;++target){
        ready("extracted alias VGET returns consistent covered identity");
        vnode_t result=NULL;CHECK(bindfs_vget(&alias_mount,nodes[target].id,&result,&caller)==0);CHECK(result==&aliases[target]);vnode_put(result);assert_balanced();
    }
    ready("alias VGET does not include object outside System tree");vnode_t result=NULL;CHECK(bindfs_vget(&alias_mount,nodes[ROOT].id,&result,&caller)==ENOENT);CHECK(!result);assert_balanced();
    ready("alias VGET underlying failure cleanup");vget_fail_node=WEB0;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EIO);CHECK(!result);assert_balanced();
    ready("alias VGET child getpath failure cleanup");getpath_fail_node=WEB0;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EIO);CHECK(!result);assert_balanced();
    ready("alias VGET lower-root getpath failure cleanup");getpath_fail_node=SYSTEM;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EIO);CHECK(!result);assert_balanced();
    ready("alias VGET lower-root iocount acquisition failure");getref_fail_node=SYSTEM;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EIO);CHECK(!result);assert_balanced();
    ready("alias VGET alias-root iocount acquisition failure");alias_root_getref_fail=true;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EIO);CHECK(!result);assert_balanced();
    ready("alias VGET relative lookup failure cleanup");lookup_fail_node=WEB0;CHECK(bindfs_vget(&alias_mount,nodes[WEB0].id,&result,&caller)==EACCES);CHECK(!result);assert_balanced();
}
static void getattr_cases(void) {
    ready("native and alias FSID64/FSID remain internally consistent");
    struct vnode_attr a;VATTR_INIT(&a);VATTR_WANTED(&a,va_fsid);VATTR_WANTED(&a,va_fsid64);VATTR_WANTED(&a,va_fileid);
    struct vnop_getattr_args args={&aliases[EXTENSIONKIT],&a,&caller};CHECK(bindfs_getattr(&args)==0);CHECK(a.va_fsid==703);CHECK(a.va_fsid64.val[0]==703&&a.va_fsid64.val[1]==57);CHECK(a.va_fileid==nodes[EXTENSIONKIT].id);
    VATTR_INIT(&a);VATTR_WANTED(&a,va_fsid);VATTR_WANTED(&a,va_fsid64);vnode_get(&nodes[EXTENSIONKIT]);CHECK(vnode_getattr(&nodes[EXTENSIONKIT],&a,&caller)==0);CHECK(a.va_fsid==501&&a.va_fsid64.val[0]==501&&a.va_fsid64.val[1]==21);vnode_put(&nodes[EXTENSIONKIT]);assert_balanced();
    ready("getattr failure preserved");getattr_fail_node=EXTENSIONKIT;VATTR_INIT(&a);VATTR_WANTED(&a,va_fsid64);CHECK(bindfs_getattr(&args)==EIO);assert_balanced();
}
static void selfcheck_cases(void) {
    ready("extracted boot selfcheck passes full/split/five-bundle contract");CHECK(selfcheck()==0);CHECK(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE);
    reset_model("boot selfcheck surfaces prepare errno");alias_mount.bind.bindm_webkit_error=EACCES;CHECK(selfcheck()==EACCES);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));
    ready("boot selfcheck lower ref error disables native view");getref_fail_node=SYSTEM;CHECK(selfcheck()==EIO);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));
    const int targets[]={LIBRARY,EXTENSIONKIT,EXTENSIONS,FRAMEWORKS,WEB0,WEB1,WEB2,WEB3,WEB4};
    for(unsigned i=0;i<9;++i){ready("boot selfcheck lookup failure cleanup");lookup_fail_node=targets[i];CHECK(selfcheck()==EACCES);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));}
    for(int i=WEB0;i<=WEB4;++i){
        ready("boot selfcheck VGET error disables native view");vget_fail_node=i;CHECK(selfcheck()==EIO);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));
        ready("boot selfcheck wrong reverse identity disables native view");wrong_reverse_node=i;CHECK(selfcheck()==EINVAL);CHECK(!(alias_mount.bind.bindm_flags&BINDM_WEBKIT_NATIVE));
    }
}
void mainCRTStartup(void) {
    prepare_cases();ordinary_cases();restriction_and_error_cases();covered_view_cases();reverse_cases();getattr_cases();selfcheck_cases();assert_balanced();
    say("PASS ");say_uint(cases);say(" cases; ");say_uint(assertions);say(" assertions. Windows host model only.\n");ExitProcess(0);
}
