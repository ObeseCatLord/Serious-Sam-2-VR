"""Offline prerequisite/ownership regressions. No display/Wine/game is launched."""
import hashlib,json,subprocess,sys,tempfile,time,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import runtime_lab as lab
import private_display_lab as display
GOOD={'schema':1,'stage':'swapchain-query','operation':'get-raster-status','desktop_hz':60,'adapter_hz':60,'swapchain_hz':60,
      'width':1280,'height':720,'raster_called':True,'cleanup_completed':True,'hresult':0,'scanline':0,'in_vblank':True}
class Checks(unittest.TestCase):
    def test_startup_snapshot_failure_is_contained_and_never_admits_success(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);record={'result':'incomplete'};owner={'pid':123}
            (root/'Sam2.log').write_text('Loading native modules\n');cfg={'startup_snapshot':True,'game_lab':str(root)}
            with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'observer',side_effect=[{'process_creation':456},TimeoutError('snapshot timeout')]) as query:
                lab.capture_startup_snapshot(cfg,{},root,record,owner,'owned-path')
            self.assertEqual([c.args[2] for c in query.call_args_list],['stock-status','stock-dump'])
            self.assertEqual(record['result'],'incomplete');self.assertEqual(record['startup_snapshot_error'],'snapshot timeout')
            self.assertNotIn('startup_snapshot',record)
            for selection,state in (({},'incomplete'),({'startup_snapshot':True},'native_neutral_captured')):
                with patch.object(lab,'observer') as query:
                    lab.capture_startup_snapshot(selection,{},root,{'result':state},owner,'owned-path')
                    query.assert_not_called()
            with patch.object(lab,'observer') as query:
                lab.capture_startup_snapshot({'startup_snapshot':True},{},root,
                    {'result':'incomplete','loaded_scene_confirmed':True},owner,'owned-path')
                query.assert_not_called()
            with patch.object(lab,'still_owned',side_effect=OSError('owner vanished')),patch.object(lab,'observer') as query:
                lab.capture_startup_snapshot(cfg,{},root,record,owner,'owned-path')
                query.assert_not_called();self.assertEqual(record['startup_snapshot_error'],'owner vanished')
            receipt=root/'startup-native-snapshot.json';receipt.write_text('{"dump_written":0,"write_error":5}')
            with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'observer',side_effect=[{'process_creation':456},RuntimeError('API refused')]) as query:
                lab.capture_startup_snapshot(cfg,{},root,record,owner,'owned-path')
            self.assertEqual(record['startup_snapshot']['write_error'],5);self.assertEqual(record['result'],'incomplete')
            self.assertEqual([c.args[2] for c in query.call_args_list],['stock-status','stock-dump'])
            self.assertEqual(record['startup_snapshot_error'],'API refused')
    def test_snapshot_excludes_world_logs_and_gameplay_before_full_vr_readiness(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg={'startup_snapshot':True,'game_lab':str(root)}
            (root/'Sam2.log').write_text("Started simulation on 'Jungle.wld'\n")
            with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'observer') as query:
                record={'result':'incomplete'};lab.capture_startup_snapshot(cfg,{},root,record,{'pid':1},'path')
                query.assert_not_called();self.assertIn('startup_snapshot_skipped',record)
            (root/'Sam2.log').write_text('Loading modules\n')
            with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'observer') as query:
                record={'result':'incomplete','world_before_vr_readiness':True}
                lab.capture_startup_snapshot(cfg,{},root,record,{'pid':1},'path');query.assert_not_called()
            with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'observer',return_value={'native_simulation':123}) as query:
                record={'result':'incomplete'};lab.capture_startup_snapshot(cfg,{},root,record,{'pid':1},'path')
                self.assertEqual(query.call_count,1);self.assertEqual(record['startup_snapshot_skipped'],'native-simulation-observed')
    def test_snapshot_timeout_rediscovery_keeps_exclusions_and_reports_unresolved_helpers(self):
        from unittest.mock import Mock
        for token in (None,'ipc-token'):
            for unresolved in (False,True):
                with self.subTest(token=token,unresolved=unresolved),tempfile.TemporaryDirectory() as d:
                    root=Path(d);(root/'Sam2.log').write_text('Loading modules\n')
                    cfg={'startup_snapshot':True,'game_lab':str(root),'proton':'proton','observer':str(root/'observer.exe')}
                    helper={'pid':2,'start':'new'};old={'pid':3,'start':'old'};remaining=[helper,old];calls=[0];stops=[0]
                    actor=Mock();actor.returncode=None;actor.poll.side_effect=lambda:actor.returncode
                    def communicate(**kw):
                        calls[0]+=1
                        if calls[0]==1:raise lab.subprocess.TimeoutExpired('observer',1)
                        return b'',b''
                    actor.communicate.side_effect=communicate;actor.terminate.side_effect=lambda:setattr(actor,'returncode',-15)
                    inventories=[0]
                    def inventory(executable,scope=None):
                        if Path(executable)!=Path(cfg['observer']):return []
                        if scope not in ('stock-path',token):return []
                        inventories[0]+=1
                        if inventories[0]==1:return [old]
                        return list(remaining)
                    def stop(item,budget=None):
                        self.assertEqual(item,helper);stops[0]+=1
                        if stops[0]==1 or unresolved:raise RuntimeError('helper retirement failed')
                        remaining.remove(helper)
                    real_observer=lab.observer
                    def query(config,env,command,key,*args,**kw):
                        if command=='stock-status':return {'process_creation':1}
                        return real_observer(config,env,command,key,*args,**kw)
                    record={'result':'incomplete'};errors=[]
                    def cleanup(action):
                        try:action()
                        except Exception as e:errors.append(str(e))
                    with patch.object(lab,'still_owned',return_value=True),patch.object(lab,'private_processes',side_effect=inventory),patch.object(lab,'stop_exact',side_effect=stop),patch.object(lab.subprocess,'Popen',return_value=actor),patch.object(lab,'observer',side_effect=query):
                        lab.capture_startup_snapshot(cfg,{},root,record,{'pid':1},'stock-path')
                        lab.retire_run_children(root,Path(cfg['observer']),token,'stock-path',{(3,'old')},lab.time.monotonic()+20,cleanup)
                    self.assertIn('startup_snapshot_error',record);self.assertEqual(stops[0],2)
                    self.assertEqual(old,remaining[-1]);self.assertEqual(bool(errors),unresolved)
                    self.assertEqual(helper in remaining,unresolved)
    def test_retirement_exclusions_survive_actual_game_observer_argument_overlap(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);exe=root/'observer.exe';game=root/'Bin/Sam2.exe';stock='Z:'+str(game)
            old={'pid':1,'start':'old','argv':[b'wine',str(exe).encode(),b'stock-status',stock.encode()]}
            helper={'pid':2,'start':'new','argv':[b'wine',str(exe).encode(),b'stock-dump',stock.encode()]}
            actor={'pid':3,'start':'game','argv':[b'wine',str(game).encode()]}
            other={'pid':4,'start':'foreign','argv':[b'wine',str(exe).encode(),b'stock-status',b'Z:/other-lab/Bin/Sam2.exe']}
            rows={str(x['pid']):x for x in (old,helper,actor,other)}
            for token in (None,'ipc-token'):
                calls=[]
                with patch.object(Path,'iterdir',return_value=[Path('/proc')/key for key in rows]),patch.object(lab,'identity',side_effect=lambda pid:rows.get(str(pid))),patch.object(lab,'stop_exact',side_effect=lambda item,budget=None:calls.append((item['pid'],item['start']))):
                    self.assertEqual({x['pid'] for x in lab.private_processes(game)},{1,2,3})
                    lab.retire_run_children(root,exe,token,stock,{(1,'old')},lab.time.monotonic()+20,lambda action:action())
                self.assertEqual(set(calls),{(2,'new'),(3,'game')});self.assertEqual(len(calls),2)
    def test_startup_snapshot_is_explicit_and_requires_neutral_runtime(self):
        for cfg in ({'startup_snapshot':1},{'startup_snapshot':'true'},
                    {'startup_snapshot':True},{'startup_snapshot':True,'neutral_runtime_probe':False},
                    {'startup_snapshot':True,'neutral_runtime_probe':True,'idle_weapon_probe':False}):
            with self.subTest(cfg=cfg),self.assertRaises(ValueError):lab.validate_idle_probe(cfg)
        lab.validate_idle_probe({'startup_snapshot':False})
        lab.validate_idle_probe({'startup_snapshot':True,'neutral_runtime_probe':True,
            'idle_weapon_probe':True,'idle_native_id':2,'expected_product_source':'a'*64,
            'baseline_head':[0,1.6,0],'pose_steps':[{'name':'baseline','head':[0,1.6,0]}]})
    def test_neutral_runtime_mode_requires_exact_stationary_prepared_game_selection(self):
        cfg={'neutral_runtime_probe':True,'idle_weapon_probe':True,'idle_native_id':2,
             'expected_product_source':'a'*64,'baseline_head':[0,1.6,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0]}]}
        lab.validate_idle_probe(cfg)
        for delta in ({'neutral_runtime_probe':1},{'neutral_runtime_probe':'true'},
                      {'idle_weapon_probe':False},{'display_prerequisite_only':True},
                      {'monado_window_order_probe':True},{'native_dual_probe':'zap-initial-inventory'},
                      {'pose_steps':[]},{'renderer_mode':'stock'}):
            with self.subTest(delta=delta),self.assertRaises(ValueError):lab.validate_idle_probe(cfg|delta)
    def test_neutral_runtime_summary_requires_scene_projection_both_eyes_and_clean_shutdown(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            private=Path(d);directory=private/'runs/fresh';directory.mkdir(parents=True)
            cfg={'private_root':str(private),'scene':{'entry_sha256':'b'*64},
                 'expected_product_source':'a'*64,'prepare_idle_fixture':True,'idle_native_id':2}
            result={'scene':cfg['scene'],'validated_config':{'expected_product_source':'a'*64},
                    'result':'native_neutral_captured','renderer_mode':'vr',
                    'game_launch_requested':True,'monado_launch_requested':True,
                    'hardware_acceptance':False,'loaded_scene_confirmed':True,
                    'native_scene_receipts':[['123','0','b'*64]],
                    'distinct_neutral_complete_pairs':30,'shutdown_observed':True,
                    'cleanup_errors':[],'termination_requests':[],
                    'idle_fixture_preparation':{'native_id':2}}
            left=directory/'baseline-left.ppm';right=directory/'baseline-right.ppm'
            Image.new('RGB',(8,8),(10,20,30)).save(left)
            Image.new('RGB',(8,8),(11,20,30)).save(right)
            receipt=directory/'result.json';receipt.write_text(json.dumps(result))
            metadata=directory/'baseline-metadata.json'
            frame={'sequence':1,'presentation':1,'ui_requested':1,'width':8,'height':8,
                   'head_clearance':{'mode':2,'valid':1,'reserved':0}}
            metadata.write_text(json.dumps(frame))
            summary=display.neutral_runtime_summary(directory,cfg)
            self.assertTrue(summary['complete']);self.assertTrue(summary['different_eye_pixels'])
            for key in ('hardware_acceptance','head_translation_verified','head_rotation_verified',
                        'physical_grasp_verified','native_release_verified'):self.assertIs(summary[key],False)
            for delta in ({'result':'display_prerequisite_only'},{'renderer_mode':'stock'},
                          {'game_launch_requested':False},{'monado_launch_requested':False},
                          {'hardware_acceptance':True},{'loaded_scene_confirmed':1},
                          {'native_scene_receipts':[]},{'native_scene_receipts':[['123','0','c'*64]]},
                          {'scene':{'entry_sha256':'c'*64}},{'validated_config':{}},
                          {'distinct_neutral_complete_pairs':29},{'distinct_neutral_complete_pairs':True},
                          {'shutdown_observed':False},{'shutdown_observed':1},{'cleanup_errors':['survivor']},
                          {'termination_requests':[15]},{'idle_fixture_preparation':{'native_id':13}}):
                receipt.write_text(json.dumps(result|delta))
                with self.subTest(delta=delta),self.assertRaises(RuntimeError):display.neutral_runtime_summary(directory,cfg)
            receipt.write_text(json.dumps(result))
            for delta in ({'head_clearance':{'mode':1,'valid':1,'reserved':0}},
                          {'head_clearance':{}},{'head_clearance':{'mode':3,'valid':1,'reserved':0}},
                          {'head_clearance':{'mode':True,'valid':1,'reserved':0}},
                          {'head_clearance':{'mode':2,'valid':0,'reserved':0}},
                          {'presentation':0},{'ui_requested':0},{'width':9}):
                metadata.write_text(json.dumps(frame|delta))
                with self.subTest(delta=delta),self.assertRaises(RuntimeError):display.neutral_runtime_summary(directory,cfg)
            metadata.write_text(json.dumps(frame))
            for mutation in ('identical','extent','truncated','symlink','missing'):
                if right.exists() or right.is_symlink():right.unlink()
                if mutation=='identical':right.write_bytes(left.read_bytes())
                elif mutation=='extent':Image.new('RGB',(9,8),(11,20,30)).save(right)
                elif mutation=='truncated':right.write_bytes(b'\x89PNG\r\n\x1a\n')
                elif mutation=='symlink':right.symlink_to(left)
                with self.subTest(mutation=mutation),self.assertRaises((RuntimeError,OSError)):
                    display.neutral_runtime_summary(directory,cfg)
    def test_neutral_completion_bypasses_optional_geometry_without_weakening_legacy(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);record={}
            self.assertTrue(lab.finish_idle_capture({'neutral_runtime_probe':True,'idle_weapon_probe':True},
                                                   root/'missing-game',root,record))
            self.assertEqual(record,{'result':'native_neutral_captured'})
            self.assertFalse(lab.finish_idle_capture({},root,root,{}))
            with self.assertRaises(FileNotFoundError):
                lab.finish_idle_capture({'idle_weapon_probe':True},root/'missing-game',root,{})
    def test_outer_neutral_receipt_matches_only_the_sealed_runtime_mode(self):
        good={'completed':True,'evidence_scope':'native-neutral-runtime',
              'neutral_runtime_complete':True,'data_ready_for_review':False}
        self.wrapper_mock(True,False,neutral=True,receipt=good,expect_success=True)
        self.wrapper_mock(True,False,expect_success=True) # Legacy qualified geometry remains accepted.
        for change in ({'completed':False},{'neutral_runtime_complete':1},
                       {'neutral_runtime_complete':False},{'evidence_scope':'geometry'},
                       {'data_ready_for_review':True}):
            self.wrapper_mock(True,False,neutral=True,receipt=good|change)
        for key in good:
            self.wrapper_mock(True,False,neutral=True,receipt={k:v for k,v in good.items() if k!=key})
        self.wrapper_mock(True,False,neutral=True) # Geometry receipt cannot pass neutral mode.
        self.wrapper_mock(True,False,receipt=good|{'data_ready_for_review':True})
    def test_eager_window_admission_requires_exact_helper_and_nongame_gui_extent(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);helper=root/'query';helper.write_bytes(b'\x7fELFfixture')
            pe=root/'probe.exe';data=bytearray(192);data[:2]=b'MZ';data[60:64]=(64).to_bytes(4,'little')
            data[64:70]=b'PE\0\0L\x01';data[88:90]=b'\x0b\x01';data[156:158]=(2).to_bytes(2,'little');pe.write_bytes(data)
            cfg={'display_prerequisite_only':True,'monado_window_order_probe':True,
                 'monado_environment':{'XRT_COMPOSITOR_DISABLE_DEFERRED':'1'},
                 'private_x11_windows':{'path':str(helper),'sha256':lab.digest(helper)},
                 'display_timing_probe':{'path':str(pe),'sha256':lab.digest(pe)}}
            lab.validate_display_probe(cfg,root)
            for change in ({'private_x11_windows':None},{'display_prerequisite_only':False},
                           {'monado_window_order_probe':1},{'monado_environment':{}},
                           {'monado_environment':{'XRT_COMPOSITOR_DISABLE_DEFERRED':'0'}},
                           {'renderer_mode':'stock'},{'display_focus_comparison':[]}):
                with self.subTest(change=change),self.assertRaises(ValueError):lab.validate_display_probe(cfg|change,root)
    def test_graphical_barrier_needs_exact_live_monado_pid_viewability_and_retained_xid(self):
        owner={'pid':123,'start':'fixed'};row={'xid':42,'pid':123,'role':1,'viewable':True,'input_output':True,'width':960,'height':540}
        with patch.object(lab,'still_owned',return_value=True):
            self.assertEqual(lab.require_owned_monado_window({'windows':[row]},owner),42)
            for rows,xid in [([],None),([row,row],None),([row|{'pid':124}],None),
                             ([row|{'viewable':False}],None),([row|{'role':2}],None),([row|{'width':0}],None),([row|{'input_output':False}],None),([row],43)]:
                with self.subTest(rows=rows,xid=xid),self.assertRaises(RuntimeError):
                    lab.require_owned_monado_window({'windows':rows},owner,xid)
        with patch.object(lab,'still_owned',return_value=False),self.assertRaises(RuntimeError):
            lab.require_owned_monado_window({'windows':[row]},owner)
    def test_private_shell_choice_is_explicit_and_preserves_default_without_arbitrary_modules(self):
        self.assertEqual(display.selected_shell({}),'kiosk')
        self.assertEqual(display.selected_shell({'private_display_shell':'desktop'}),'desktop')
        for value in ('desktop-shell.so','../../custom',None,True,[],{}):
            with self.subTest(value=value),self.assertRaises(ValueError):display.selected_shell({'private_display_shell':value})
    def test_child_interval_needs_fresh_owner_coverage_not_post_observation_samples(self):
        def rows(ticks):return [{'tick_ms':tick} for tick in ticks]
        before,during,after,covered=lab.foreground_interval(rows(range(90,371,20)),100,350)
        self.assertTrue(covered);self.assertEqual(before['tick_ms'],90)
        self.assertTrue(all(100<row['tick_ms']<=350 for row in during));self.assertEqual(after['tick_ms'],370)
        for ticks,entry,observed in [([90,360,370],100,350),([0,110,130,350,370],100,350),
                                     ([90,110,130,350,500],100,350),([110,130,350,370],100,350),
                                     ([90,110,130,350],100,350),([90,110,130,350,370],350,100)]:
            with self.subTest(ticks=ticks):self.assertFalse(lab.foreground_interval(rows(ticks),entry,observed)[3])
    def test_foreground_comparison_requires_sealed_nongame_gui_owner_and_exact_variant_subsystems(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            def executable(name,subsystem):
                path=root/name;data=bytearray(192);data[:2]=b'MZ';data[60:64]=(64).to_bytes(4,'little')
                data[64:70]=b'PE\0\0L\x01';data[88:90]=b'\x0b\x01';data[156:158]=subsystem.to_bytes(2,'little')
                path.write_bytes(data);return {'path':str(path),'sha256':hashlib.sha256(data).hexdigest()}
            cfg={'display_prerequisite_only':True,'display_timing_probe':executable('owner.exe',2),
                 'display_focus_comparison':[{'name':'console',**executable('console.exe',3)},
                                             {'name':'gui',**executable('gui.exe',2)}]}
            lab.validate_display_probe(cfg,root)
            for change in ({'display_prerequisite_only':False},{'display_focus_comparison':[]},
                           {'display_focus_comparison':list(reversed(cfg['display_focus_comparison']))},
                           {'display_timing_probe':executable('wrong-owner.exe',3)}):
                with self.subTest(change=change),self.assertRaises(ValueError):lab.validate_display_probe(cfg|change,root)
    def test_owner_foreground_samples_require_complete_bounded_monotonic_native_rows(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'owner.jsonl';row={'tick_ms':1,'foreground_hwnd':100,'foreground_owner':32,
                'foreground_thread':36,'probe_title_matches':True}
            p.write_text(json.dumps(row)+'\n'+json.dumps(row|{'tick_ms':2})+'\n'+ '{"tick_ms":')
            self.assertEqual(lab.owner_foreground_samples(p),[row,row|{'tick_ms':2}])
            for data in ('',json.dumps(row|{'tick_ms':True})+'\n',json.dumps(row)+'\n'+json.dumps(row|{'tick_ms':0})+'\n',
                         json.dumps(row|{'foreground_owner':-1})+'\n','x'*131073):
                p.write_text(data)
                with self.assertRaises((RuntimeError,json.JSONDecodeError)):lab.owner_foreground_samples(p)
    def test_native_activation_records_success_rejection_and_missing_receipt(self):
        for code,present in [(0,True),(8,True),(8,False),(0,False)]:
            with self.subTest(code=code,present=present),tempfile.TemporaryDirectory() as d:
                receipt=Path(d)/'activation.json';deadline=time.monotonic()+5
                native={'stage':'window-owner' if code else 'foreground-observed','returncode':code}
                def attempt(cfg,env,command,token,output,**kw):
                    self.assertEqual(command,'stock-focus');self.assertEqual(token,'owned-game')
                    self.assertEqual(output,(123,'Z:'+str(receipt)))
                    self.assertEqual(kw['deadline'],deadline)
                    if present:receipt.write_text(json.dumps(native))
                    if code:raise lab.ObserverError(command,code)
                with patch.object(lab,'observer',side_effect=attempt) as operation:
                    record=lab.recorded_native_focus({}, {},'owned-game',123,receipt,deadline,timeout=20)
                    operation.assert_called_once()
                    self.assertGreater(operation.call_args.kwargs['timeout'],3)
                    self.assertLessEqual(operation.call_args.kwargs['timeout'],5)
                self.assertEqual(record['observer_returncode'],code)
                self.assertEqual(record['receipt_present'],present)
                if present:self.assertEqual(record['native_receipt'],native)
                else:self.assertNotIn('native_receipt',record)
    def test_native_activation_preserves_fatal_codes_and_refuses_colliding_receipts(self):
        with tempfile.TemporaryDirectory() as d:
            receipt=Path(d)/'activation.json';deadline=time.monotonic()+5
            with patch.object(lab,'observer',side_effect=lab.ObserverError('stock-focus',6)):
                with self.assertRaises(lab.ObserverError) as error:
                    lab.recorded_native_focus({}, {},'owned-game',123,receipt,deadline)
                self.assertEqual(error.exception.code,6)
            receipt.symlink_to(Path(d)/'missing')
            with patch.object(lab,'observer') as operation:
                with self.assertRaisesRegex(RuntimeError,'already exists'):
                    lab.recorded_native_focus({}, {},'owned-game',123,receipt,deadline)
                operation.assert_not_called()
    def test_zero_current_mode_failure_or_forged_success_is_not_readiness(self):
        lab.require_display_timing(GOOD)
        for key,value in [('schema',True),('stage','invalid-current-mode'),('raster_called',1),
                          ('operation','create-device'),('cleanup_completed',False),('hresult',False),('hresult',2289436780),('swapchain_hz',0),
                          ('adapter_hz',1),('desktop_hz',0),('width',False),('height',16385)]:
            with self.subTest(key=key),self.assertRaises(RuntimeError):lab.require_display_timing(GOOD|{key:value})
    def test_private_display_environment_removes_desktop_routes_and_ownership_is_exact(self):
        original={'DISPLAY':':1','WAYLAND_DISPLAY':'wayland-0','XAUTHORITY':'desktop',
                  'DBUS_SESSION_BUS_ADDRESS':'desktop','MIR_SOCKET':'desktop','OTHER':'keep'}
        env=display.private_environment(original,Path('/private/runtime'),'token')
        self.assertEqual(original['DISPLAY'],':1');self.assertEqual(env['OTHER'],'keep')
        for key in original:
            if key!='OTHER':self.assertNotIn(key,env)
        raw=b'SS2VR_PRIVATE_CAPTURE_TOKEN=token\0SS2VR_LAB_DISPLAY_ROOT=/private/runtime\0'
        self.assertTrue(display.tagged(raw,'token','/private/runtime'))
        for value in (raw.replace(b'token',b'other'),raw.replace(b'/private/runtime',b'/run/user'),
                      raw+b'SS2VR_PRIVATE_CAPTURE_TOKEN=token\0',b''):
            self.assertFalse(display.tagged(value,'token','/private/runtime'))
    def test_pinned_probe_requires_private_x86_executable(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);exe=root/'probe.exe';data=bytearray(70);data[:2]=b'MZ';data[60:64]=(64).to_bytes(4,'little');data[64:70]=b'PE\0\0L\x01';exe.write_bytes(data)
            cfg={'display_timing_probe':{'path':str(exe),'sha256':hashlib.sha256(data).hexdigest()}}
            lab.validate_display_probe(cfg,root)
            data[68:70]=b'd\x86';exe.write_bytes(data);cfg['display_timing_probe']['sha256']=hashlib.sha256(data).hexdigest()
            with self.assertRaises(ValueError):lab.validate_display_probe(cfg,root)
            with self.assertRaises(ValueError):lab.validate_display_probe({'display_timing_probe':True},root)
    def test_failed_or_timed_out_probe_prevents_progress_and_retires_exact_child(self):
        for fault in ('zero-mode','timeout','bad-status','duplicate-json'):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as d:
                root=Path(d);exe=root/'probe.exe';exe.write_bytes(b'pinned fixture');calls=[]
                cfg={'proton':'not-launched','display_timing_probe':{'path':str(exe),'sha256':lab.digest(exe)}}
                class Process:
                    code=None
                    def poll(self):return self.code
                    def wait(self,timeout):
                        if fault=='timeout' and self.code is None:raise subprocess.TimeoutExpired('mock',timeout)
                        self.code=0 if fault!='bad-status' else 5;return self.code
                    def terminate(self):calls.append('term');self.code=-15
                    def kill(self):calls.append('kill');self.code=-9
                process=Process()
                def popen(args,**kw):
                    self.assertEqual(args[1],'runinprefix')
                    self.assertEqual(kw['env']['PROTON_LOG'],'0')
                    report=GOOD|({'swapchain_hz':0} if fault=='zero-mode' else {})
                    text=json.dumps(report)+'\n'
                    if fault=='duplicate-json':text*=2
                    Path(kw['env']['SS2VR_DISPLAY_TIMING_OUTPUT'][2:]).write_text(text);return process
                discovers=[[],[{'pid':123}]]
                with patch.object(lab.subprocess,'Popen',side_effect=popen),patch.object(lab,'private_processes',side_effect=discovers),patch.object(lab,'stop_exact',side_effect=lambda item,budget:calls.append(item['pid'])):
                    with self.assertRaises((RuntimeError,ValueError,subprocess.TimeoutExpired)):
                        lab.display_timing_probe(cfg,{'SS2VR_LAB_PRIVATE_DISPLAY':'1','DISPLAY':':2'},lab.time.monotonic()+40,root)
                self.assertIn(123,calls);self.assertFalse((root/'display-timing.json').exists())
                if fault=='timeout':self.assertIn('term',calls)
    def test_probe_report_write_failure_cannot_skip_exact_retirement(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);exe=root/'probe.exe';exe.write_bytes(b'pinned');calls=[]
            cfg={'proton':'never-run','display_timing_probe':{'path':str(exe),'sha256':lab.digest(exe)}}
            env={'SS2VR_LAB_PRIVATE_DISPLAY':'1','DISPLAY':':2','PROTON_LOG':'1'}
            class Process:
                def poll(self):return 0
                def wait(self,timeout):return 0
            def popen(args,**kw):
                self.assertEqual(kw['env']['PROTON_LOG'],'0')
                Path(kw['env']['SS2VR_DISPLAY_TIMING_OUTPUT'][2:]).write_text(json.dumps(GOOD));return Process()
            original=Path.write_text
            def write(path,*args,**kw):
                if path.name=='display-timing-process.json':raise OSError('injected diagnostic write failure')
                return original(path,*args,**kw)
            with patch.object(lab.subprocess,'Popen',side_effect=popen),patch.object(lab,'private_processes',side_effect=[[],[{'pid':99}]]),patch.object(lab,'stop_exact',side_effect=lambda p,budget:calls.append(p['pid'])),patch.object(Path,'write_text',write):
                with self.assertRaisesRegex(RuntimeError,'cleanup'):
                    lab.display_timing_probe(cfg,env,lab.time.monotonic()+40,root)
            self.assertEqual(calls,[99]);self.assertEqual(env['PROTON_LOG'],'1')
    def test_native_receipt_not_console_bait_certifies_readiness(self):
        for bait in ('missing','symlink','oversize'):
            with self.subTest(bait=bait),tempfile.TemporaryDirectory() as d:
                root=Path(d);exe=root/'probe.exe';exe.write_bytes(b'pinned');cfg={'proton':'never-run','display_timing_probe':{'path':str(exe),'sha256':lab.digest(exe)}}
                class Process:
                    def poll(self):return 0
                    def wait(self,timeout):return 0
                def popen(args,**kw):
                    kw['stdout'].write((json.dumps(GOOD)+'\n').encode());kw['stdout'].flush()
                    target=Path(kw['env']['SS2VR_DISPLAY_TIMING_OUTPUT'][2:])
                    if bait=='symlink':target.symlink_to(exe)
                    if bait=='oversize':target.write_text('x'*4097)
                    return Process()
                with patch.object(lab.subprocess,'Popen',side_effect=popen),patch.object(lab,'private_processes',return_value=[]):
                    with self.assertRaises(RuntimeError):lab.display_timing_probe(cfg,{'SS2VR_LAB_PRIVATE_DISPLAY':'1','DISPLAY':':2'},lab.time.monotonic()+40,root)
                self.assertFalse((root/'display-timing.json').exists())
    def test_desktop_selector_or_wayland_never_reaches_proton(self):
        with patch.object(lab.subprocess,'Popen') as process:
            for env in ({'DISPLAY':':1'},{'SS2VR_LAB_PRIVATE_DISPLAY':'1','DISPLAY':':2','WAYLAND_DISPLAY':'wayland-0'}):
                with self.assertRaises(RuntimeError):lab.display_timing_probe({'display_timing_probe':{}},env,0,Path('.'))
            process.assert_not_called()
    def test_lazy_xwayland_starts_before_ownership_and_display_only_never_imports_collector(self):
        self.inner_mode(False)
    def test_prerequisite_wrapper_never_imports_idle_collector_or_claims_geometry(self):
        self.inner_mode(True)
    def test_neutral_runtime_uses_shared_run_without_loading_optional_geometry_collector(self):
        self.inner_mode(False,True)
    def inner_mode(self, prerequisite, neutral=False):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);out=root/'display-check';out.mkdir();runtime=root/'runtime';runtime.mkdir()
            cfg=root/'config.json';cfg.write_text(json.dumps({'private_root':str(root)}))
            parent={'pid':456,'start':'cookie','argv':[b'weston']}
            (out/'owner.json').write_text(json.dumps({'pid':456,'start':'cookie','argv':['weston'],'runtime':str(runtime)}))
            started=[False];events=[];own_pid=display.os.getpid()
            def identify(pid):
                if int(pid)==456:return parent
                if int(pid)==own_pid:return {'start':'self'}
                events.append('server-check')
                if started[0]:return {'pid':789,'parent':456,'name':'Xwayland','argv':[b'Xwayland',b':2'],'start':'x'}
                return None
            def query(args,**kw):
                self.assertEqual(args,['xrandr','--current']);started[0]=True;events.append('x-client')
                return subprocess.CompletedProcess(args,0,b'positive-refresh',b'')
            original=Path.iterdir
            def list_paths(path):return iter([Path('/proc/1')]) if str(path)=='/proc' else original(path)
            env=display.private_environment({},runtime,'token')|{'DISPLAY':':2','WAYLAND_DISPLAY':'ss2-vr-private'}
            directory=root/'runs/diagnostic';directory.mkdir(parents=True)
            (directory/'result.json').write_text(json.dumps({'result':'display_prerequisite_only','cleanup_errors':[], 'game_launch_requested':False,'monado_launch_requested':False}))
            prepared={'display_prerequisite_only':True} if prerequisite else {'neutral_runtime_probe':True}
            with patch.object(display,'runtime_base',return_value=root),patch.object(display,'prepared_configuration',return_value=prepared),patch.object(display,'run',return_value=directory) as native,patch.object(display.signal,'signal') as signals,patch.object(display,'neutral_runtime_summary',return_value={'evidence_scope':'native-neutral-runtime'}) as summary,patch.dict(display.os.environ,env,clear=True),patch.object(display.os,'getppid',return_value=456),patch.object(display,'identity',side_effect=identify),patch.object(Path,'iterdir',list_paths),patch.object(display.subprocess,'run',side_effect=query),patch.object(display.importlib.util,'spec_from_file_location') as load:
                display.inside(cfg,out,not (prerequisite or neutral),prerequisite);load.assert_not_called()
                if prerequisite or neutral:
                    native.assert_called_once();self.assertEqual(signals.call_args.args[0],display.signal.SIGTERM)
                else:native.assert_not_called()
                if neutral:summary.assert_called_once_with(directory,prepared)
                else:summary.assert_not_called()
            self.assertLess(events.index('x-client'),events.index('server-check'))
            outcome=json.loads((out/'collection-outcome.json').read_text())
            self.assertFalse(outcome['data_ready_for_review'])
            if neutral:self.assertTrue(outcome['neutral_runtime_complete'])
            else:
                self.assertTrue(outcome['prerequisite_only' if prerequisite else 'display_only']);self.assertFalse(outcome['game_launched'])
    def test_check_cannot_be_combined_with_display_launch(self):
        with tempfile.TemporaryDirectory() as d:
            cfg=Path(d)/'config.json';cfg.write_text('{}')
            with patch.object(display.sys,'argv',['tool','--config',str(cfg),'--check','--display-only']),patch.object(display,'prepared_configuration',return_value={'display_timing_probe':True}),patch.object(display,'launch') as launch:
                with self.assertRaises(SystemExit) as error:display.main()
                self.assertEqual(error.exception.code,2);launch.assert_not_called()
    def test_prerequisite_mode_needs_pinned_probe_and_cannot_select_game(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg=root/'config.json';cfg.write_text('{}')
            with self.assertRaises(ValueError):lab.validate_display_probe({'display_prerequisite_only':True},root)
            with self.assertRaises(ValueError):lab.validate_display_probe({'display_prerequisite_only':1},root)
            for arguments,prepared in [(['--run'],{'display_prerequisite_only':True}),
                                       (['--prerequisite-only'],{'display_prerequisite_only':False}),
                                       (['--check','--inside',str(root)],{})]:
                with patch.object(display.sys,'argv',['tool','--config',str(cfg),*arguments]),patch.object(display,'prepared_configuration',return_value=prepared),patch.object(display,'launch') as launch,patch.object(display,'inside') as inside:
                    with self.assertRaises(SystemExit):display.main()
                    launch.assert_not_called();inside.assert_not_called()
    def test_prerequisite_actual_run_extent_stops_before_service_or_game_even_after_readiness(self):
        for failure in (False,True):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as d:
                root=Path(d);game=root/'game';prefix=root/'prefix';prefix.mkdir()
                path=game/'Bin/SS2VR/install-receipt.json';path.parent.mkdir(parents=True);path.write_text('{}')
                cfg={'display_prerequisite_only':True,'online_isolation':'native-init-barrier-verified',
                     'observer':'unused-observer','fingerprints':{},'scene':{'entry':'private-fixture'},
                     'startup':{},'steam':'unused-steam','native_manifest':'unused-manifest',
                     'xrizer':'unused-xrizer','runtime_libraries':'unused-libraries','monado_environment':{}}
                def checked(args,**kw):return 'a'*40 if kw.get('text') else b''
                with patch.object(lab,'validate',return_value=(root,game,prefix)),patch.object(lab,'running',return_value=[]),patch.object(lab,'private_processes',return_value=[]),patch.object(lab.subprocess,'check_output',side_effect=checked),patch.object(lab.subprocess,'Popen') as actors,patch.object(lab,'display_timing_probe',side_effect=RuntimeError('native fixture failure') if failure else None,return_value=GOOD):
                    if failure:
                        with self.assertRaisesRegex(RuntimeError,'native fixture failure'):lab.run(cfg)
                    else:directory=lab.run(cfg)
                    actors.assert_not_called()
                directory=next((root/'runs').iterdir());record=json.loads((directory/'result.json').read_text())
                self.assertEqual(record['cleanup_errors'],[]);self.assertFalse(record['shutdown_observed'])
                if not failure:
                    self.assertEqual(record['result'],'display_prerequisite_only')
                    self.assertFalse(record['game_launch_requested']);self.assertFalse(record['monado_launch_requested'])
                self.assertFalse((directory/'config/monado').exists());self.assertFalse((directory/'launch.log').exists())
    def test_overlong_socket_path_refuses_before_any_process_or_attempt(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)/('long-fixture-name-'*4);root.mkdir()
            cfg={'private_root':str(root),'display_timing_probe':True,'expected_product_source':'a'*64}
            # The socket directory comes from runtime_base(); an overlong one must refuse.
            with patch.object(display,'runtime_base',return_value=root),patch.object(display.subprocess,'Popen') as process:
                with self.assertRaisesRegex(RuntimeError,'socket path'):display.launch(root/'config.json',cfg)
                process.assert_not_called()
            self.assertFalse((root/'agent-capture-attempt.json').exists())
    def test_weston_exit_without_collector_receipt_is_not_success(self):
        self.wrapper_mock(False,False)
    def test_wrapper_preexec_certificate_and_cleanup_survive_failure(self):
        self.wrapper_mock(True,True)
    def test_late_cancellation_is_interrupted_after_complete_cleanup(self):
        self.wrapper_mock(True,False,True)
    def wrapper_mock(self, completed, fail_cleanup, late=False, neutral=False, receipt=None, expect_success=False):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg={'private_root':str(root),'display_timing_probe':True,'expected_product_source':'a'*64}
            if neutral:cfg['neutral_runtime_probe']=True
            (root/'config.json').write_text(json.dumps(cfg));command=[];calls=[];live={101,102};identified=[0]
            class Process:
                pid=100;returncode=0
                def poll(self):return 0
            def popen(cmd,**kw):
                command[:]=cmd
                self.assertNotIn('DISPLAY',kw['env']);self.assertNotIn('WAYLAND_DISPLAY',kw['env'])
                self.assertEqual(Path(kw['env']['XDG_RUNTIME_DIR']).parent,root)
                self.assertLess(len((kw['env']['XDG_RUNTIME_DIR']+'/ss2-vr-private').encode()),108)
                if completed:(root/'private-display/collection-outcome.json').write_text(json.dumps(receipt if receipt is not None else {'completed':True,'data_ready_for_review':True}))
                return Process()
            def identify(pid):
                identified[0]+=1
                return {'pid':100,'start':'cookie','argv':[b'python-preexec'] if identified[0]==1 else [part.encode() for part in command]}
            def gather(owned,*args):
                for pid in live:owned[(pid,'cookie')]={'pid':pid,'start':'cookie','argv':[b'owned-helper']}
            faults=[fail_cleanup];handlers={};cancel=[late]
            def signals(sig,handler):
                old=handlers.get(sig);handlers[sig]=handler;return old
            def stop(item,deadline):
                calls.append(item['pid'])
                if cancel[0]:
                    cancel[0]=False;handlers[display.signal.SIGTERM](display.signal.SIGTERM,None)
                if faults[0]:faults[0]=False;raise RuntimeError('injected child cleanup fault')
                live.discard(item['pid'])
            with patch.object(display,'runtime_base',return_value=root),patch.object(display.subprocess,'Popen',side_effect=popen),patch.object(display,'identity',side_effect=identify),patch.object(display,'gather',side_effect=gather),patch.object(display,'still_owned',side_effect=lambda item:item['pid'] in live),patch.object(display,'stop_exact',side_effect=stop),patch.object(display.signal,'signal',side_effect=signals),patch.object(display.time,'sleep'):
                if expect_success:display.launch(root/'config.json',cfg)
                else:
                    with self.assertRaises((FileNotFoundError,RuntimeError)):
                        display.launch(root/'config.json',cfg)
            report=json.loads((root/'private-display/result.json').read_text())
            self.assertEqual(report['survivors'],[]);self.assertTrue(report['display_stopped'])
            self.assertTrue((root/'agent-capture-attempt.json').exists())
            self.assertEqual(live,set());self.assertIn(102,calls)
            if fail_cleanup:self.assertTrue(report['cleanup_errors'])
            elif late:
                self.assertTrue(report['deferred_signals']);self.assertEqual(report['cleanup_errors'],[])
            elif expect_success:self.assertNotIn('error',report)
            else:self.assertIn('error',report)
            certificate=json.loads((root/'private-display/owner.json').read_text())
            self.assertEqual(certificate['argv'],command)
            with self.assertRaises(FileExistsError):display.launch(root/'config.json',cfg)
if __name__=='__main__':unittest.main()
