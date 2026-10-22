"""Guarded disposable Jolt allocation probes; does not grant production admission."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct
import subprocess
import time
from output_paths import evidence_directory
from source_manifest import ROOT, authored_inputs, file_sha256, git_state
from verify_desktop_surfaces import native_guard


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-receipt',type=Path,required=True)
    parser.add_argument('--configuration',choices=['Release','Debug'],required=True)
    parser.add_argument('--expected-source-commit',required=True)
    parser.add_argument('--expected-authored-input-sha256',required=True)
    args=parser.parse_args()
    out=evidence_directory('p04-allocator-probe-'+args.configuration.lower())
    checks=[];calls=[];observation=None;artifacts={};passed=False;error=None;include_scan=None
    def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))
    def check(name,value):
        checks.append(dict(check=name,passed=bool(value)))
        if not value:raise RuntimeError(name)
    def current():
        state=git_state()
        check('exact clean current commit',state['commit']==args.expected_source_commit and state['working_tree_clean'])
        check('exact current authored inputs',authored_inputs()['sha256']==args.expected_authored_input_sha256)
    def execute(option=None):
        guard=native_guard(out)
        with (out/f'launch-{len(calls)+1:02d}-guard.json').open('x',encoding='utf-8') as f:json.dump(guard,f,indent=2)
        check('resource/serialization guard',guard['passed'])
        check('unchanged executable before launch',file_sha256(executable)==artifacts[str(executable)])
        command=[str(executable)]+([option] if option else [])
        start=time.perf_counter()
        run=subprocess.run(command,capture_output=True,text=True,errors='replace',timeout=120)
        calls.append(dict(command=command,exit_code=run.returncode,seconds=time.perf_counter()-start,stdout=run.stdout,stderr=run.stderr))
        return run
    try:
        current()
        authority=read(args.build_receipt.resolve(strict=True))
        check('successful matching isolated build',authority['passed'] and authority['configuration']==args.configuration and
              authority['source_commit']==args.expected_source_commit and authority['authored_input_sha256']==args.expected_authored_input_sha256)
        executable=Path(authority['executable']).resolve(strict=True)
        check('isolated build cache executable',executable.is_relative_to(ROOT/'out') and executable.name=='race_jolt_allocation_probe.exe')
        artifacts=authority['artifacts']
        for path,digest in artifacts.items():check('build artifact '+path,file_sha256(Path(path))==digest)
        from physics_build_overlay import LOCK, PATCH, SOURCE, derive
        dependency=read(executable.parent/'dependency-identity.json')
        overlay=dependency['build_overlay'];lock=read(LOCK)
        original=Path(dependency['source_root'])/SOURCE;derived=Path(overlay['derived'])
        check('explicit immutable upstream and derived SDK identities',overlay['patch']==PATCH and
              file_sha256(original)==lock['upstream_sha256'] and file_sha256(derived)==lock['derived_sha256'] and
              derived.read_bytes()==derive(original.read_bytes(),lock))
        commands=read(executable.parent/'compile_commands.json')
        tree_commands=[c for c in commands if Path(c['file']).name=='QuadTree.cpp']
        check('exactly the derived tree TU compiled',len(tree_commands)==1 and Path(tree_commands[0]['file']).resolve()==derived.resolve())
        import physics_epa_overlay as epa
        epa_lock=read(epa.LOCK);epa_overlay=dependency['epa_build_overlay']
        epa_original=Path(dependency['source_root'])/epa.SOURCE;epa_derived=Path(epa_overlay['derived']).resolve(strict=True)
        check('separately pinned EPA header derivation',epa_overlay['patch']==epa.PATCH and
              file_sha256(epa_original)==epa_lock['upstream_sha256'] and file_sha256(epa_derived)==epa_lock['derived_sha256'] and
              epa_derived.read_bytes()==epa.derive(epa_original.read_bytes(),epa_lock))
        include_root=Path(epa_overlay['include_root']).resolve(strict=True)
        check('EPA include root stays in build cache',include_root.is_relative_to(executable.parent/'generated') and
              epa_derived==include_root/epa.SOURCE)
        compiled=[c for c in commands if any(part in c['output'].replace('\\','/') for part in
                  ('/Jolt.dir/','/race_jolt_allocation_probe.dir/'))]
        check('relevant compile commands present',len(compiled)>1)
        exception_sources={ROOT/p for p in ('tests/physics_qualification/owned_leases_probe.cpp',
            'tests/physics_qualification/product_owner_probe.cpp','src/assets/collision_wire.cpp','src/assets/digest.cpp')}
        owner_commands=[c for c in compiled if Path(c['file']).resolve() in exception_sources]
        check('allocation exceptions isolated to RACE composition sources',len(owner_commands)==4 and all('/EHsc' in c['command'] for c in owner_commands))
        check('SDK and main probe remain exception-free',all('/EHsc' not in c['command'] for c in compiled if Path(c['file']).resolve() not in exception_sources))
        for c in compiled:
            command=c['command'].replace('\\','/').replace('"','')
            overlay_index=command.find('-I'+include_root.as_posix())
            original_index=command.find('-I'+Path(dependency['source_root']).as_posix())
            check('EPA include precedence '+Path(c['file']).name,0<=overlay_index<original_index)
        guard=native_guard(out)
        with (out/'dependency-scan-guard.json').open('x',encoding='utf-8') as f:json.dump(guard,f,indent=2)
        check('dependency scan resource/serialization guard',guard['passed'])
        ninja=Path('C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
        cache=(executable.parent/'CMakeCache.txt').read_text()
        check('pinned dependency scanner matches build',('CMAKE_MAKE_PROGRAM:FILEPATH='+ninja.as_posix()) in cache)
        scan=subprocess.run([str(ninja),'-t','deps'],cwd=executable.parent,capture_output=True,text=True,errors='replace',timeout=30)
        with (out/'compiler-dependencies.log').open('x',encoding='utf-8') as f:f.write(scan.stdout+scan.stderr)
        check('compiler dependency scan succeeds',scan.returncode==0)
        blocks={};current_object=None
        for line in scan.stdout.splitlines():
            if ': #deps ' in line:
                current_object=line.split(': #deps ',1)[0].replace('\\','/')
                blocks[current_object]=dict(valid=line.endswith('(VALID)'),includes=[])
            elif line.startswith('    ') and current_object:
                blocks[current_object]['includes'].append(line.strip())
        routed=[]
        for c in compiled:
            obj=Path(c['output']).resolve().relative_to(executable.parent).as_posix()
            block=blocks.get(obj)
            check('current compiler dependencies '+Path(c['file']).name,block is not None and block['valid'])
            # Ninja records dependencies relative to its build working directory.
            matches=[(executable.parent/Path(p)).resolve(strict=True) for p in block['includes']
                     if Path(p).name=='EPAConvexHullBuilder.h']
            if matches:
                check('actual derived EPA consumption '+Path(c['file']).name,all(p==epa_derived for p in matches))
                routed.append(c['file'])
        check('collision and probe consume derived EPA',any(Path(p).name=='ConvexShape.cpp' for p in routed) and
              any(Path(p).name=='allocation_probe.cpp' for p in routed))
        include_scan=dict(command=[str(ninja),'-t','deps'],exit_code=scan.returncode,
                          log_sha256=file_sha256(out/'compiler-dependencies.log'),consumers=routed,
                          derived_header=str(epa_derived),derived_sha256=epa_lock['derived_sha256'])
        raw=executable.read_bytes();pe=struct.unpack_from('<I',raw,0x3c)[0]
        check('Windows x64 PE',raw[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',raw,pe+4)[0]==0x8664)
        check('explicit 1 MiB stack reservation',struct.unpack_from('<Q',raw,pe+24+72)[0]==1024**2)
        run=execute();check('normal probe exit',run.returncode==0)
        observation=json.loads(run.stdout)
        check('probe succeeds without claiming allocation proof',observation['passed'] and observation['simulation_executed'] and not observation['allocation_proved'])
        check('minimal registry with no retained SDK allocations',not observation['factory_registry_used'] and observation['sdk_heap_live_at_shutdown']==0)
        check('approved total includes the reserved stack',observation['reserved_bytes']==64*1024**2)
        check('scratch observation within proposed reservation',0<observation['scratch_peak_bytes']<=16*1024**2)
        check('heap observation within probe reservation',0<observation['sdk_heap_peak_bytes']<=42*1024**2)
        check('no intercepted CRT new storage',observation['crt_new_requests']==observation['crt_new_peak_bytes']==observation['crt_new_live_bytes']==0)
        check('declared workload counts',observation['freefall_ticks']==8 and observation['stack_ticks']==2400 and observation['dense_stress_ticks']==128)
        check('discrete freefall reference',observation['freefall_position_error_m']<=.0001 and observation['freefall_velocity_error_m_s']<=.0001)
        run=execute('--sdk-broadphase-depth')
        check('adversarial tree placement corrected',run.returncode==0 and json.loads(run.stdout)['tree_case_passed'])
        run=execute('--sdk-epa-capacity');check('EPA fixed-pool capacity fixture exit',run.returncode==0)
        capacity=json.loads(run.stdout)
        check('EPA unchanged pool and approved output capacities',capacity['epa_capacity_passed'] and capacity['triangles']==256 and
              capacity['points']==128 and capacity['edges']==capacity['new_triangles']==256 and capacity['entry_layout_bytes']==16 and capacity['derived_array_bytes']==4096)
        check('EPA additional horizon array storage',capacity['horizon_array_delta_bytes']==3072 and
              capacity['edges_container_bytes']==4104 and capacity['new_triangles_container_bytes']==2056)
        check('EPA capacity fixture scope',capacity['synthetic_capacity_fixture'] and not capacity['physical_depth_reproduction'] and not capacity['allocation_proved'])
        run=execute('--owned-records');check('owned record fixture exit',run.returncode==0)
        owned=json.loads(run.stdout)
        check('actual typed record capacities',owned['owned_records_passed'] and owned['body_bytes']<=576 and
              owned['command_bytes']<=640 and owned['identity_bytes']<=128 and owned['snapshot_bytes']<=192 and
              owned['pose_bytes']<=192 and owned['event_bytes']<=288 and owned['hit_bytes']<=144 and owned['stamp_bytes']+16<=256)
        check('actual typed arrays fit service reservation',owned['workspace_bytes']+131072+1048576<=6*1024**2 and
              owned['workspace_alignment']==16 and owned['phase_bytes']<=256*192+4096*288+4096 and owned['query_bytes']<=256*144+256)
        check('owned value lifetime and explicit qualification limits',owned['copied_hits']==16384 and owned['captured_events']==4096 and
              not owned['control_blocks_qualified'] and not owned['product_validation_qualified'] and not owned['allocation_proved'])
        run=execute('--sdk-query-operand');check('serialized temporary query operand exit',run.returncode==0)
        operand=json.loads(run.stdout)
        check('temporary shape ABI fits approved placement slot',operand['query_operand_passed'] and operand['world_shapes']==256 and
              operand['temporary_slots']==1 and operand['temporary_storage_bytes']==512 and operand['box_bytes']==80 and
              operand['sphere_bytes']==56 and operand['box_alignment']<=16 and operand['sphere_alignment']<=16)
        check('temporary query ownership and teardown',operand['captured_hits']==512 and operand['reentrant_rejections']==512 and
              operand['sdk_heap_live_at_shutdown']==operand['crt_new_requests']==0 and not operand['allocation_proved'])
        run=execute('--owned-leases');check('owned output lease fixture exit',run.returncode==0)
        leases=json.loads(run.stdout)
        check('actual output control blocks fit reserved slots',leases['owned_leases_passed'] and leases['account_control_bytes']<=65536 and
              leases['query_control_and_value_bytes']<=256*144+256 and leases['phase_control_and_value_bytes']<=256*192+4096*288+4096 and
              leases['pool_metadata_bytes']+64*(256*144+256)+(256*192+4096*288+4096)<=4*1024**2)
        check('retained output lifetime and accounting',leases['control_block_allocations']==leases['control_block_frees']==68 and
              leases['peak_charged_bytes']==64*1024**2 and leases['retained_charged_bytes']==4*1024**2+65536 and
              leases['retained_account_regions']==leases['retained_output_regions']==0)
        check('output ownership qualification limits',leases['qualification_only'] and not leases['solver_executed'] and
              leases['concurrent_release_tested'] and leases['reader_stack_reservation']==65536 and not leases['allocation_proved'])
        run=execute('--owned-product');check('validated owned collision product exit',run.returncode==0)
        product=json.loads(run.stdout)
        check('owned product uses real codec and rejects stale or invalid inputs',product['product_owner_passed'] and product['product_bytes']==312 and product['rejections']==11 and product['codec_reused'])
        check('owned product lifetime and qualification scope',product['source_borrow_released'] and product['pending_retired'] and product['old_product_preserved'] and
              not product['whole_batch_qualified'] and not product['solver_executed'] and not product['allocation_proved'])
        run=execute('--sdk-contact-ownership');check('real SDK callback ownership exit',run.returncode==0)
        contact=json.loads(run.stdout)
        check('copied contact lifecycle and sleeping sensor suppression',contact['contact_owner_passed'] and contact['added']>=3 and contact['persisted']>=3 and
              contact['removed']>=2 and contact['sensor_sleep_removals']>=1 and contact['seeded_overflow_rejected'])
        check('callback fixed storage and teardown',contact['state_bytes']<=5*1024**2 and contact['listener_bytes']<=64 and
              contact['sdk_heap_live_at_shutdown']==contact['crt_new_requests']==0)
        check('callback qualification limits',contact['single_thread_only'] and not contact['trigger_reconciliation_qualified'] and
              not contact['scene_publication_qualified'] and not contact['allocation_proved'])
        run=execute('--sdk-trigger-ownership');check('geometric trigger ownership exit',run.returncode==0)
        trigger=json.loads(run.stdout)
        check('bounded pair work and sleeping continuity',trigger['trigger_owner_passed'] and trigger['native_pairs']==27 and trigger['max_pair_tests']==32640 and
              trigger['pair_table_bytes']==32640 and trigger['state_bytes']<=5*1024**2 and trigger['sleeping_persists'])
        check('trigger replacement filtering overflow and teardown',trigger['replacement_and_filter_tested'] and trigger['overflow_rejected'] and
              trigger['sdk_heap_live_at_shutdown']==trigger['crt_new_requests']==0)
        check('trigger qualification limits',not trigger['numeric_boundary_qualified'] and not trigger['scene_publication_qualified'] and not trigger['allocation_proved'])
        run=execute('--composed-ownership');check('joint SDK and owned output execution',run.returncode==0)
        composed=json.loads(run.stdout)
        check('joint fixed capacity execution',composed['composed_ownership_passed'] and composed['solver_executed'] and composed['bodies']==256 and composed['moving_bodies']==128 and
              composed['ticks']==8 and composed['retained_queries']==composed['copied_hits']==64 and composed['copied_poses']==256 and composed['events']>=2 and composed['pair_tests']==255)
        check('joint reservations and retained owner charge',composed['control_used_bytes']<=composed['control_reserved_bytes']==960*1024 and
              composed['peak_charged_bytes']==64*1024**2 and composed['retained_with_owner_stack_bytes']==5*1024**2+65536 and
              composed['sdk_peak_bytes']<=42*1024**2 and composed['scratch_peak_bytes']<=16*1024**2)
        check('joint teardown and qualification limits',composed['sdk_heap_live_at_shutdown']==composed['crt_new_requests']==0 and
              not composed['whole_batch_qualified'] and not composed['scene_publication_qualified'] and not composed['allocation_proved'])
        run=execute('--sdk-query-contracts');check('query probe exit',run.returncode==0)
        queries=json.loads(run.stdout)
        check('full retained query outputs, filters and explicit overflow',queries['query_case_passed'] and
              queries['queries']==64 and queries['owned_hits']==16384 and queries['filter_hits']==128 and queries['overflow_rejected'])
        run=execute('--sdk-scratch-envelope');check('synthetic maximum scratch fixture exit',run.returncode==0)
        scratch=json.loads(run.stdout)
        check('complete synthetic contact/island allocation envelope',scratch['scratch_envelope_passed'] and
              scratch['synthetic_allocation_fixture'] and scratch['requests']==17 and
              scratch['peak_bytes']==16473472 and scratch['reservation_bytes']==16*1024**2 and not scratch['allocation_proved'])
        check('complete scratch splitter lifecycle and teardown',scratch['split_islands']==128 and
              scratch['synthetic_contact_links']==32768 and scratch['velocity_contact_visits']==360448 and
              scratch['position_contact_visits']==65536 and scratch['scratch_live_at_shutdown']==0)
        run=execute('--sdk-storage-bounds');check('storage ABI and lifetime fixture exit',run.returncode==0)
        storage=json.loads(run.stdout)
        check('actual query and allocator ABI',storage['storage_bounds_passed'] and storage['query_hit_bytes']==16 and
              storage['query_hit_alignment']==8 and storage['query_storage_bytes']==262144 and
              storage['query_storage_location']=='stack' and storage['arena_block_bytes']==64 and storage['arena_block_alignment']==16)
        check('allocator residue and split-boundary coverage',storage['arena_charge_cases']==3584 and
              storage['per_block_round16_overhead_bound']==272)
        check('storage scope and complete fixture teardown',storage['sdk_heap_live_at_shutdown']==storage['crt_new_requests']==0 and
              not storage['allocation_proved'] and not storage['sites']['allocation_sites_complete'])
        for site,count in [('contact_cache',6),('body_manager',5),('island_links',1),('primitive_shapes',2),('rigid_body_variants',4),('job_slots',2)]:
            allocations=storage['sites'][site]
            check('bounded allocation site '+site,len(allocations)==count and all(
                a['requested']>0 and a['alignment'] in (16,32,64,128) and
                a['requested']<=a['charged']<=((a['requested']+15)&~15)+272 for a in allocations))
        run=execute('--sdk-mutable-lifetimes');check('mutable allocation lifetime fixture exit',run.returncode==0)
        lifetime=json.loads(run.stdout)
        check('cache lifetime count differs from body capacity',lifetime['cache_registered_bodies']==256 and
              lifetime['cache_distinct_lifetimes']==288 and lifetime['cache_capacity_entries']==512 and lifetime['cache_allocation_requests']==10)
        check('old/new cache allocation overlap',lifetime['cache_overlap_charged_bytes']>3072 and lifetime['cache_peak_delta_bytes']>=2048)
        check('tree rebuild, abort and reclamation lifecycles',lifetime['tree_cycles']==32 and lifetime['tree_slot_capacity']==512 and
              lifetime['tree_slot_bytes']==128 and lifetime['tree_pages_prewarmed'] and 2<lifetime['tree_observed_max_live_nodes']<=512)
        check('mutable fixture teardown and open admission',lifetime['mutable_lifetimes_passed'] and
              lifetime['sdk_heap_live_at_shutdown']==lifetime['crt_new_requests']==0 and not lifetime['allocation_proved'])
        for site,count in [('broadphase_initialization',4),('create_before_register_rejection',1),('unassigned_bodies',2),('shape_replacement_overlap',2)]:
            check('lifetime site '+site,len(lifetime['sites'][site])==count)
        run=execute('--sdk-mutation-protocol');check('production-path mutation protocol fixture exit',run.returncode==0)
        protocol=json.loads(run.stdout)
        check('paused and idle paths retain invalidation admission accounting',protocol['protocol_passed'] and
              protocol['paused_invalidation_hold'] and protocol['no_active_invalidation_hold'] and
              protocol['cache_admission_ceiling']==256 and protocol['cache_requests']==9 and protocol['active_steps']==2)
        check('pending and retained ownership admission',protocol['pending_body_ceiling']==protocol['owned_shape_ceiling']==256)
        check('full two-layer paused maintenance',protocol['paused_two_layer_cycles']==32 and protocol['maintenance_calls']>=96)
        check('protocol scope and complete teardown',protocol['qualification_only'] and not protocol['allocation_proved'] and
              protocol['sdk_heap_live_at_shutdown']==protocol['crt_new_requests']==0)
        run=execute('--sdk-deferred-edits');check('deferred-edit owner fixture exit',run.returncode==0)
        deferred=json.loads(run.stdout)
        check('bounded replaceable and ordered paused edits',deferred['deferred_edits_passed'] and deferred['paused_assignments']==4096 and
              deferred['coalesced_assignments']==4095 and deferred['queue_capacity']==256 and deferred['ordered_operations']==5)
        check('large job pages prewarmed before all deferred owners',deferred['prewarmed_job_pages']==5)
        check('positive idle path and SDK identity wrap',deferred['idle_replacement_ticks']==512 and deferred['sdk_id_reuses']>=2 and deferred['idle_updates']>=512)
        check('both-layer replacement and retired contact identity',deferred['full_two_layer_replacements']==32 and deferred['retired_contact_id_wraps']==1)
        check('deferred fixture complete teardown and open allocation gate',deferred['qualification_only'] and not deferred['allocation_proved'] and
              deferred['sdk_heap_live_at_shutdown']==deferred['crt_new_requests']==0)
        run=execute('--sdk-wrapper-storage');check('public wrapper allocation fixture exit',run.returncode==0)
        wrapper=json.loads(run.stdout)
        check('wrapper AddState ABI',wrapper['wrapper_storage_passed'] and wrapper['add_state_bytes']==48 and wrapper['add_state_alignment']==16)
        for site,count in [('wrapper_prepare_abort',3),('wrapper_prepare_finalize',3),('wrapper_full_rebuild',4),
                           ('wrapper_partial_layer_0',2),('wrapper_partial_layer_1',2),('wrapper_clean_update',0),('wrapper_empty_reclamation',0)]:
            check('wrapper lifetime '+site,len(wrapper['sites'][site])==count)
        check('wrapper fixture complete teardown and open allocation gate',wrapper['sdk_heap_live_at_shutdown']==wrapper['crt_new_requests']==0 and not wrapper['allocation_proved'])
        run=execute('--sdk-traversal-storage');check('synthetic traversal storage fixture exit',run.returncode==0)
        traversal=json.loads(run.stdout)
        check('explicit synthetic capacity scope',traversal['traversal_storage_passed'] and traversal['synthetic_capacity_fixture'] and
              traversal['frontier_bound']==768 and traversal['capacity_bound']==1024 and not traversal['allocation_proved'])
        for site,count in [('rebuild_spill',5),('ray_spill',6),('validation_spill',2)]:
            check('traversal allocation site '+site,len(traversal['sites'][site])==count)
        check('source-derived traversal charge bounds',traversal['rebuild_observed_overlap']<=traversal['rebuild_charge_bound']==9696 and
              traversal['ray_observed_overlap']<=traversal['ray_charge_bound']==10720 and
              traversal['validation_observed_overlap']<=traversal['validation_charge_bound']==6464)
        check('traversal complete teardown',traversal['sdk_heap_live_at_shutdown']==traversal['crt_new_requests']==0)
        run=execute('--sdk-startup-storage');check('startup storage fixture exit',run.returncode==0)
        startup=json.loads(run.stdout)
        check('startup storage scope',startup['startup_storage_passed'] and startup['split_bits_synthetic'] and not startup['allocation_proved'])
        for site,count in [('primitive_registration_material',2),('physics_system_initialization',17),('split_validation_bits',1)]:
            check('startup allocation site '+site,len(startup['sites'][site])==count)
        check('startup complete teardown',startup['sdk_heap_live_at_shutdown']==startup['crt_new_requests']==0)
        run=execute('--sdk-paused-empty-shortage')
        check('ordinary paused maintenance does not reclaim dirty empty layers',run.returncode!=0 and
              'QuadTree: Out of nodes!' in run.stderr and 'did not terminate' not in run.stderr)
        run=execute('--sdk-tree-churn-shortage')
        check('unmaintained batch churn exhausts only its child',run.returncode!=0 and
              'QuadTree: Out of nodes!' in run.stderr and 'did not terminate' not in run.stderr)
        for option,marker in [('--sdk-scratch-shortage','TempAllocator: Out of memory'),('--sdk-scratch-order','TempAllocator: Freeing in the wrong order'),('--arena-shortage','SDK arena exhausted'),('--job-shortage','SDK assertion' if args.configuration=='Debug' else None)]:
            run=execute(option)
            check(option+' terminates only its disposable child',run.returncode!=0 and 'did not terminate' not in run.stderr)
            if marker:check(option+' exact rejection diagnostic',marker in run.stderr)
        for path,digest in artifacts.items():check('unchanged final artifact '+path,file_sha256(Path(path))==digest)
        from physics_dependencies import verify
        from output_paths import project_root
        check('complete original dependency tree unchanged after children',verify(project_root())['passed'])
        check('derived SDK source unchanged after children',file_sha256(derived)==lock['derived_sha256'])
        check('derived EPA header unchanged after children',file_sha256(epa_derived)==epa_lock['derived_sha256'])
        current();passed=True
    except Exception as failure:error=f'{type(failure).__name__}: {failure}'
    receipt=dict(schema=1,passed=passed,error=error,source_commit=args.expected_source_commit,
        authored_input_sha256=args.expected_authored_input_sha256,configuration=args.configuration,
        build_receipt=str(args.build_receipt),checks=checks,calls=calls,observation=observation,artifacts=artifacts,
        script_sha256=file_sha256(Path(__file__)),include_scan=include_scan,allocation_proved=False,normal_application_integration=False,
        scope='Isolated measured memory/numerical corroboration and fatal child probes; no exhaustive source/lifetime/fragmentation/stack proof or full P04 acceptance.')
    with (out/'receipt.json').open('x',encoding='utf-8') as f:json.dump(receipt,f,indent=2)
    print(json.dumps(dict(passed=passed,error=error,receipt=str(out/'receipt.json'))))
    return 0 if passed else 1


if __name__=='__main__':raise SystemExit(main())
