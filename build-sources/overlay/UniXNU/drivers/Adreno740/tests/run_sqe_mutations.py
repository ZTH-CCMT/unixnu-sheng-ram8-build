# SPDX-License-Identifier: MIT
"""Negative controls for V5.3. Mutate generated HOST copies, never candidate sources.
Requires the same native Windows clang/SDK fixture as build/run-submit-host.py.
The mutation must reach a named test assertion, not merely fail to compile.
"""
import pathlib, subprocess, sys
base = pathlib.Path(__file__).resolve().parents[3]
runner = base / "build/run-submit-host.py"
mutations = {
    "equal-pointer-gate": ("NativeAdrenoPreempt", 
        "evidence_.sqe_status=io_.sqe_snapshot?io_.sqe_snapshot(io_.cookie,&evidence_.sqe):-1;",
        "evidence_.sqe_status=io_.sqe_snapshot && evidence_.registers[6]!=evidence_.registers[7]?io_.sqe_snapshot(io_.cookie,&evidence_.sqe):-1;",
        "test_submit", "cp.sqe_calls==1", "--timeout-evidence"),
    "software-next-boundary": ("NativeAdrenoRing",
        "const uint32_t unpublished=(next_-out->hw_wptr)&mask;",
        "out->hw_wptr=next_; const uint32_t unpublished=0;",
        "test_ring", "tail.hw_rptr==published", None),
    "unpublished-wrap-inclusion": ("NativeAdrenoRing",
        "const uint32_t available=out->ring_words-unpublished;",
        "const uint32_t available=out->ring_words;",
        "test_ring", "tail.count==3", None),
    "failed-read-valid-zero": ("NativeAdrenoPreempt",
        "if(!evidence_.register_status[i]) {evidence_.registers[i]=value;evidence_.register_valid_mask|=1u<<i;}",
        "{evidence_.registers[i]=value;evidence_.register_valid_mask|=1u<<i;}",
        "test_preempt", "out.register_status[2]==17", None),
    "overwrite-frozen-slot": ("NativeAdrenoSubmit",
        "index>=__atomic_load_n(&switch_evidence_ready,__ATOMIC_ACQUIRE))return 0;",
        "index>=__atomic_load_n(&switch_evidence_ready,__ATOMIC_ACQUIRE))return 0; switch_evidence[index].sequence=native_adreno_switch_count;",
        "test_submit", "raw,expected_evidence[slot]", "--timeout-evidence"),
}
if len(sys.argv)==3 and sys.argv[1]=="--one":
    key=sys.argv[2];source,old,new,test,_,arg=mutations[key]
    code=runner.read_text()
    code=code.replace('out = base / "build/gpu-tests-current"',
                      'out = base / "build/gpu-tests-sqe-mutant-'+key+'"')
    oldline="    p.write_text(code)"
    inject=("    if name == "+repr(source)+":\n"
            "        assert "+repr(old)+" in code\n"
            "        code=code.replace("+repr(old)+","+repr(new)+")\n"+oldline)
    code=code.replace(oldline,inject)
    # Keep the existing real source/dependency compilation, run just the target.
    code=code[:code.index('run_test("test_preempt",')]
    deps={"test_preempt":["NativeAdrenoPreempt"],
          "test_ring":["NativeAdrenoRing","NativeAdrenoPreempt"],
          "test_submit":["NativeAdrenoMemory","NativeAdrenoSmmuMemory","NativeAdrenoVm", "NativeAdrenoRing","NativeAdrenoPreempt","NativeAdrenoSubmit","HostKernelHooks"]}[test]
    obj="[]" if test=="test_preempt" else "[objects[-1]]" if test=="test_ring" else "objects"
    # Skip default multi-mode invocation; fail at the specific negative control.
    start=code.index('    run([exe])')
    code=code[:start]+('    run([exe,'+repr(arg)+'])\n' if arg else '    run([exe])\n')
    code+='\nrun_test('+repr(test)+','+repr(deps)+','+obj+')\n'
    exec(compile(code,str(runner),"exec"),{"__file__":str(runner),"__name__":"__main__"})
    raise SystemExit(0)
for key,(_,_,_,_,needle,_) in mutations.items():
    result=subprocess.run([sys.executable,__file__,"--one",key],capture_output=True,text=True)
    if result.returncode==0 or needle not in result.stdout+result.stderr:
        print(result.stdout);print(result.stderr)
        raise SystemExit("Mutation did not reach expected failing assertion: "+key)
    print("EXPECTED FAILURE",key,":",needle,flush=True)
print("PASS all V5.3 negative controls failed at their intended assertions")
