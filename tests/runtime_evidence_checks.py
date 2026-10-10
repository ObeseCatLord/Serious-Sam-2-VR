"""Offline negative controls for request correlation; never runtime VR proof."""
import sys
import unittest
import hashlib
import json
import tempfile
from unittest.mock import patch
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from assess_runtime import (complete_pairs_for_pose,first_person_depth_probe,dual_topologies,
    DualPhaseEvidence,require_dual_capture,dual_weapon_events,successful_dual_fire)
from runtime_lab import (native_grip_resource_receipts,validate_idle_probe,validate_idle_preparation,idle_probe_weapon,validate_sniper_destination,sniper_preparation_receipt,same_owned_native_game,
                         preparation_weapon,validate_fixture_destination,fixture_preparation_receipt,
                         idle_configuration_digest,IDLE_FIXED_FILES,IDLE_TOOLS)
from build_contract import OBSERVER_FORMAT, OBSERVER_FIELDS, read_observer_layout, validate_observer_layout
from private_display_checks import Checks as PrivateDisplayChecks

HEAD={'p':[0,0,0],'q':[0,0,0,1]}

class ObserverLogChecks(unittest.TestCase):
    def test_observer_cannot_overwrite_game_proton_log(self):
        from runtime_lab import observer
        cfg={'proton':'private-proton','observer':'private-observer'}
        env={'PROTON_LOG':'1','PROTON_LOG_DIR':'private-run','PRIVATE_MARKER':'owned'}
        with patch('runtime_lab.private_processes',return_value=[]),patch('runtime_lab.subprocess.Popen') as spawn:
            spawn.return_value.communicate.return_value=(b'',b'')
            spawn.return_value.returncode=0
            observer(cfg,env,'shutdown','owned-token')
            sent=spawn.call_args.kwargs['env']
            self.assertEqual(sent,{**env,'PROTON_LOG':'0'})
            self.assertIsNot(sent,env)
            self.assertEqual(env['PROTON_LOG'],'1')
            self.assertEqual(spawn.call_args.args[0],['private-proton','runinprefix','private-observer','shutdown','owned-token'])

class ObserverLayoutChecks(unittest.TestCase):
    def setUp(self):
        self.values=[1,0x32565253,11,272,440,352,33554928,83887888]
        self.export=SimpleNamespace(name=b'ss2vrObserverLayout',address=0x1000,forwarder=None)
        self.section=SimpleNamespace(Characteristics=0x40000040,VirtualAddress=0x1000,
                                     SizeOfRawData=32,Misc_VirtualSize=32)

    def read(self,values=None,changes=None,contract=None):
        values=self.values if values is None else values
        pe=SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x14c),
            OPTIONAL_HEADER=SimpleNamespace(Magic=0x10b,Subsystem=2),
            DIRECTORY_ENTRY_EXPORT=SimpleNamespace(symbols=[self.export]),
            get_section_by_rva=lambda _:self.section,
            get_data=lambda *_:OBSERVER_FORMAT.pack(*values))
        for name,value in (changes or {}).items():setattr(pe,name,value)
        with patch('build_contract.pefile.PE') as factory:
            factory.return_value.__enter__.return_value=pe
            return read_observer_layout('private-observer.exe') if contract is None else \
                validate_observer_layout('private-observer.exe',contract)

    def test_old_layout_and_each_product_field_decline(self):
        contract=dict(zip(OBSERVER_FIELDS,self.values))
        self.assertEqual(self.read(contract=contract),contract)
        for index in range(2,8):
            values=self.values.copy();values[index]+=1
            with self.subTest(field=OBSERVER_FIELDS[index]),self.assertRaises(ValueError):
                self.read(values=values,contract=contract)
        for index in range(8):
            values=self.values.copy();values[index]=0
            with self.subTest(zero=OBSERVER_FIELDS[index]),self.assertRaises(ValueError):self.read(values=values)

    def test_missing_forwarded_duplicate_and_wrong_pe_decline(self):
        for symbols in ([],[self.export,self.export],
                        [SimpleNamespace(name=self.export.name,address=0x1000,forwarder=b'other.export')]):
            with self.subTest(symbols=len(symbols)),self.assertRaises(ValueError):
                self.read(changes={'DIRECTORY_ENTRY_EXPORT':SimpleNamespace(symbols=symbols)})
        for change in ({'FILE_HEADER':SimpleNamespace(Machine=0x8664)},
                       {'OPTIONAL_HEADER':SimpleNamespace(Magic=0x10b,Subsystem=3)},
                       {'OPTIONAL_HEADER':SimpleNamespace(Magic=0x20b,Subsystem=2)},
                       {'get_section_by_rva':lambda _:None},
                       {'get_data':lambda *_:b'truncated'}):
            with self.assertRaises(ValueError):self.read(changes=change)

    def test_unbounded_writable_or_executable_export_declines(self):
        for name,value in (('Characteristics',0xc0000040),('Characteristics',0x60000040),
                           ('Characteristics',0x40),('VirtualAddress',0x1001),
                           ('SizeOfRawData',31),('Misc_VirtualSize',31)):
            before=getattr(self.section,name);setattr(self.section,name,value)
            with self.subTest(name=name,value=value),self.assertRaises(ValueError):self.read()
            setattr(self.section,name,before)

class IdleProbeSelectionChecks(unittest.TestCase):
    def test_native_windows_pid_is_independent_of_linux_ownership(self):
        owned={'pid':100000,'start':'stable-linux-incarnation','argv':[b'owned-private-game']}
        with patch('runtime_lab.still_owned',return_value=True) as check:
            self.assertTrue(same_owned_native_game(328,{'game_pid':328},owned))
            check.assert_called_with(owned)
            for expected,state in ((328,{'game_pid':329}),(0,{'game_pid':0}),
                                   (True,{'game_pid':1}),(1,{'game_pid':True})):
                self.assertFalse(same_owned_native_game(expected,state,owned))
        with patch('runtime_lab.still_owned',return_value=False):
            self.assertFalse(same_owned_native_game(328,{'game_pid':328},owned))

    def test_stationary_pinned_collection_and_mode_separation(self):
        cfg={'idle_weapon_probe':True,'expected_product_source':'a'*64,
             'baseline_head':[0,1.6,0,0,0,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0,0,0,0]}]}
        validate_idle_probe(cfg)
        for key,value in [('idle_weapon_probe',1),('renderer_mode','stock'),('expected_product_source','a'),
                          ('native_dual_probe','zap-initial-inventory'),('background_controls_probe','joystick-native-movement'),
                          ('native_input_probe','read-only-three-samples'),('grip_resource_probe',True),
                          ('pose_steps',cfg['pose_steps']+[{'name':'turned','head':[0,1.6,0,1,0,0]}])]:
            with self.subTest(key=key),self.assertRaises(ValueError):validate_idle_probe({**cfg,key:value})
        validate_idle_probe({})

    def test_exact_idle_weapon_selector_and_disabled_rejection(self):
        cfg={'idle_weapon_probe':True,'expected_product_source':'a'*64,
             'baseline_head':[0,1.6,0,0,0,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0,0,0,0]}]}
        self.assertEqual(idle_probe_weapon(cfg),1)
        self.assertIsNone(idle_probe_weapon({}))
        for selected in (1,2,13):
            with self.subTest(selected=selected):
                validate_idle_probe({**cfg,'idle_native_id':selected})
                self.assertEqual(idle_probe_weapon({**cfg,'idle_native_id':selected}),selected)
        for selected in (True,False,0,3,14,13.0,'13',None):
            with self.subTest(selected=selected),self.assertRaises(ValueError):
                validate_idle_probe({**cfg,'idle_native_id':selected})
        for enabled in (False,None):
            disabled={'idle_native_id':13}
            if enabled is not None:disabled['idle_weapon_probe']=enabled
            with self.assertRaises(ValueError):validate_idle_probe(disabled)

    def test_sniper_preparation_needs_exact_neutral_selector(self):
        cfg={'idle_weapon_probe':True,'idle_native_id':13,'prepare_sniper_fixture':True,
             'expected_product_source':'a'*64,'baseline_head':[0,1.6,0,0,0,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0,0,0,0]}]}
        validate_idle_probe(cfg)
        for change in ({'idle_native_id':1},{'idle_weapon_probe':False},
                       {'prepare_sniper_fixture':1},{'prepare_sniper_fixture':None},
                       {'native_dual_probe':'zap-initial-inventory'}):
            with self.subTest(change=change),self.assertRaises(ValueError):validate_idle_probe({**cfg,**change})

    def test_alternate_eye_order_requires_neutral_id13(self):
        cfg={'idle_weapon_probe':True,'idle_native_id':13,'idle_alternate_world_eyes':True,
             'expected_product_source':'a'*64,'baseline_head':[0,1.6,0,0,0,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0,0,0,0]}]}
        validate_idle_probe(cfg)
        for change in ({'idle_native_id':1},{'idle_weapon_probe':False},
                       {'idle_alternate_world_eyes':1},{'idle_alternate_world_eyes':None},
                       {'native_dual_probe':'zap-initial-inventory'},
                       {'pose_steps':[{'name':'turn','head':[0,1.6,0,1,0,0]}]}):
            with self.subTest(change=change),self.assertRaises(ValueError):validate_idle_probe({**cfg,**change})

    def test_generic_preparation_selector_and_legacy_conflict(self):
        cfg={'idle_weapon_probe':True,'idle_native_id':2,'prepare_idle_fixture':True,
             'expected_product_source':'a'*64,'baseline_head':[0,1.6,0,0,0,0],
             'pose_steps':[{'name':'baseline','head':[0,1.6,0,0,0,0]}]}
        for id in (2,13):
            selected={**cfg,'idle_native_id':id}
            validate_idle_probe(selected);self.assertEqual(preparation_weapon(selected),id)
        for change in ({'idle_native_id':1},{'idle_weapon_probe':False},{'prepare_idle_fixture':1},
                       {'prepare_idle_fixture':None},{'prepare_sniper_fixture':True},
                       {'native_dual_probe':'zap-initial-inventory'}):
            with self.subTest(change=change),self.assertRaises(ValueError):validate_idle_probe({**cfg,**change})

    def test_autosg_receipt_cannot_borrow_sniper_target_or_owner(self):
        with tempfile.TemporaryDirectory() as d:
            lab=Path(d)/'game';destination=lab/'Temp/SS2VR';destination.mkdir(parents=True)
            validate_fixture_destination(lab,2)
            path=destination/'autosg-id2.sav';path.write_bytes(b'private synthetic save')
            with self.assertRaises(ValueError):validate_fixture_destination(lab,2)
            validate_sniper_destination(lab) # Different target; never overwrite either.
            win='Z:'+str(lab).replace('/','\\')+'\\Temp\\SS2VR\\autosg-id2.sav'
            log=''.join('Lab autosg preparation issued stage='+stage+' owner=12\n' for stage in ('grant','select','save'))
            log+='Lab autosg preparation complete owner=12 nativeId=2 path='+win+'\n'
            report=fixture_preparation_receipt(log,lab,2)
            self.assertEqual(report['native_id'],2)
            self.assertFalse(report['alignment_accepted']);self.assertFalse(report['save_reload_verified'])
            for bad in (log.replace('nativeId=2','nativeId=13'),log.replace('autosg-id2.sav','sniper-id13.sav'),
                        log.replace('stage=select owner=12','stage=select owner=13'),log+log,
                        log+'Lab autosg preparation failed reason=owner\n'):
                with self.subTest(log=bad),self.assertRaises(ValueError):fixture_preparation_receipt(bad,lab,2)
            with self.assertRaises(ValueError):sniper_preparation_receipt(log,lab)
            path.unlink();path.symlink_to(destination/'missing')
            with self.assertRaises(ValueError):fixture_preparation_receipt(log,lab,2)

    def test_sniper_save_destination_collision_and_redirection(self):
        with tempfile.TemporaryDirectory() as d:
            lab=Path(d)/'game';lab.mkdir();destination=lab/'Temp/SS2VR'
            with self.assertRaises(ValueError):validate_sniper_destination(lab)
            destination.mkdir(parents=True);validate_sniper_destination(lab)
            for name in ('sniper-id13.sav','sniper-id13.sav.preload'):
                p=destination/name;p.write_bytes(b'preserve')
                with self.assertRaises(ValueError):validate_sniper_destination(lab)
                self.assertEqual(p.read_bytes(),b'preserve');p.unlink()
                p.symlink_to(destination/'absent')
                with self.assertRaises(ValueError):validate_sniper_destination(lab)
                p.unlink()
            destination.rmdir();destination.symlink_to(Path(d))
            with self.assertRaises(ValueError):validate_sniper_destination(lab)

    def test_sniper_receipt_requires_order_owner_and_actual_private_files(self):
        with tempfile.TemporaryDirectory() as d:
            lab=Path(d)/'game';destination=lab/'Temp/SS2VR';destination.mkdir(parents=True)
            path=destination/'sniper-id13.sav';path.write_bytes(b'offline test fixture')
            win='Z:'+str(lab).replace('/','\\')+'\\Temp\\SS2VR\\sniper-id13.sav'
            grant='Lab sniper preparation issued stage=grant owner=12\n'
            select='Lab sniper preparation issued stage=select owner=12\n'
            save='Lab sniper preparation issued stage=save owner=12\n'
            complete='Lab sniper preparation complete owner=12 nativeId=13 path='+win+'\n'
            log=grant+select+save+complete
            report=sniper_preparation_receipt(log,lab)
            self.assertEqual(report['files']['Temp/SS2VR/sniper-id13.sav']['sha256'],hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertFalse(report['alignment_accepted']);self.assertFalse(report['save_reload_verified'])
            for bad in (select+grant+save+complete,log+complete,log.replace('stage=save owner=12','stage=save owner=13'),
                        log.replace(win,'Z:\\wrong\\sniper-id13.sav'),log+'Lab sniper preparation failed reason=owner\n'):
                with self.subTest(log=bad),self.assertRaises(ValueError):sniper_preparation_receipt(bad,lab)
            companion=destination/'sniper-id13.sav.preload';companion.write_bytes(b'private companion')
            self.assertEqual(len(sniper_preparation_receipt(log,lab)['files']),2)
            companion.unlink();companion.symlink_to(destination/'missing')
            with self.assertRaises(ValueError):sniper_preparation_receipt(log,lab)
            companion.unlink();path.write_bytes(b'')
            with self.assertRaises(ValueError):sniper_preparation_receipt(log,lab)

class IdlePreparationChecks(unittest.TestCase):
    def prepared(self,root):
        game=root/'game';prefix=root/'prefix'
        for relative in IDLE_FIXED_FILES:
            p=root/relative;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'private-fixture')
        users=prefix/'pfx/drive_c/users/steamuser';users.mkdir(parents=True)
        (users/'Documents').mkdir();(users/'My Documents').symlink_to('Documents')
        mappings=prefix/'pfx/dosdevices';mappings.mkdir()
        (mappings/'c:').symlink_to('../drive_c');(mappings/'z:').symlink_to('/')
        cfg={'idle_weapon_probe':True,'expected_product_source':'a'*64,'game_lab':str(game),'prefix':str(prefix)}
        record={'schema':1,'game_lab':str(game),'prefix':str(prefix),'source_fingerprint':'a'*64,
                'configuration_sha256':idle_configuration_digest(cfg),
                'user_links':{'prefix/pfx/drive_c/users/steamuser/My Documents':'Documents'},
                'files':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in IDLE_FIXED_FILES},
                'source_tools':{name:hashlib.sha256((Path(__file__).resolve().parents[1]/'tools'/name).read_bytes()).hexdigest() for name in IDLE_TOOLS}}
        path=root/'preparation.json'
        def seal():
            path.write_text(json.dumps(record));cfg['idle_preparation']={'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        seal()
        return cfg,record,seal,game,prefix

    def test_prepared_fixture_and_configuration_tampering(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg,record,seal,game,prefix=self.prepared(root)
            validate_idle_preparation(cfg,root,game,prefix)
            validate_idle_preparation({**cfg,'compiled_product_contract':{},'verified_scene_providers':[]},root,game,prefix)
            for bad in ({**cfg,'timeout':180},{**cfg,'idle_weapon_probe':False},
                        {**cfg,'idle_native_id':13},
                        {k:v for k,v in cfg.items() if k!='idle_weapon_probe'},
                        {**cfg,'idle_weapon_probe':False,'native_dual_probe':'zap-initial-inventory'},
                        {**cfg,'idle_preparation':{}},
                        {**cfg,'idle_preparation':{**cfg['idle_preparation'],'sha256':'0'*64}}):
                with self.assertRaises(ValueError):validate_idle_preparation(bad,root,game,prefix)
            record['source_tools']['assess_idle_weapon.py']='0'*64;seal()
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)

    def test_settings_inventory_and_profile_changes(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg,record,seal,game,prefix=self.prepared(root)
            p=game/'Content/SeriousSam2/Config/new.cfg';p.parent.mkdir(parents=True);p.write_text('changed')
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            p.unlink();p=game/'Content/PlayerProfiles/profile.dat';p.parent.mkdir();p.write_bytes(b'profile')
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            p.unlink();(game/'Bin/SS2VR/SS2VR.ini').write_bytes(b'changed')
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)

    def test_external_user_redirection_and_device_mapping(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cfg,record,seal,game,prefix=self.prepared(root)
            redirect=prefix/'pfx/drive_c/users/steamuser/My Documents'
            redirect.unlink();redirect.symlink_to(root)
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            redirect.unlink();redirect.symlink_to('./Documents')
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            redirect.unlink()
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            redirect.symlink_to('Documents')
            extra=redirect.parent/'Another Alias';extra.symlink_to('Documents')
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            extra.unlink()
            mapping=prefix/'pfx/dosdevices/e:';mapping.symlink_to(root)
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)
            mapping.unlink();record['files']['../escape']='a'*64;seal()
            with self.assertRaises(ValueError):validate_idle_preparation(cfg,root,game,prefix)


class ResourceReceiptChecks(unittest.TestCase):
    @staticmethod
    def receipts():
        return '\n'.join(f'Lab native grip resource key={key} bytes=100 positionRestored=0 sha256={key:064x}' for key in range(1,6))

    def test_complete_opens_and_repeated_identical_version(self):
        log=self.receipts()
        values=native_grip_resource_receipts(log+'\n'+log.splitlines()[0])
        self.assertEqual(len(values),5)
        self.assertEqual(values[0]['open_count'],2)
        self.assertEqual(values[-1]['resource'],'R_Hand.bmf')

    def test_partial_conflicting_saturated_or_invalid_receipts_fail(self):
        log=self.receipts()
        conflicts=log+'\n'+log.splitlines()[0].replace('bytes=100','bytes=101')
        for bad in ('',log.splitlines()[0],conflicts,
                    log+'\nLab native grip resource saturated key=1',
                    log+'\n'+log.splitlines()[0]+'\n'+log.splitlines()[0],
                    log.replace('positionRestored=0','positionRestored=101',1),
                    log.replace('bytes=100','bytes=33554433',1),
                    log.replace('key=5','key=6',1)):
            with self.subTest(bad=bad),self.assertRaises(ValueError):
                native_grip_resource_receipts(bad)

def fixture(count=30,camera_sequence_offset=0,camera_tracking=7,presentation=1,eyes=(0,1)):
    game=[];host=[]
    for sequence in range(1,count+1):
        for eye in eyes:
            game.append(f'Lab native camera eye={eye} request={sequence+camera_sequence_offset} session=2 reference=3 tracking={camera_tracking} head=0,0,0,0,0,0,1 camera={eye*.064},0,0,0,0,0,1')
        game.append(f'Lab native pair request={sequence} session=2 reference=3 tracking=7 presentation={presentation}')
        host.append(f'Lab projection submitted request={sequence} session=2 reference=3 tracking=7 presentation={presentation}')
    return '\n'.join(game),'\n'.join(host)

class CorrelationChecks(unittest.TestCase):
    def test_distinct_complete_requests(self):
        self.assertEqual(len(complete_pairs_for_pose(*fixture(),HEAD)),30)

    def test_same_pose_other_requests_cannot_certify_capture(self):
        self.assertFalse(complete_pairs_for_pose(*fixture(camera_sequence_offset=100),HEAD))

    def test_repeated_submission_is_one_native_pair(self):
        game,host=fixture(count=1)
        self.assertEqual(len(complete_pairs_for_pose(game,host*100,HEAD)),1)

    def test_generation_and_session_are_not_interchangeable(self):
        self.assertFalse(complete_pairs_for_pose(*fixture(camera_tracking=8),HEAD))
        game,host=fixture()
        self.assertFalse(complete_pairs_for_pose(game,host.replace('session=2','session=4'),HEAD))
        self.assertFalse(complete_pairs_for_pose(game,host.replace('reference=3','reference=4'),HEAD))

    def test_world_only_or_single_eye_is_incomplete(self):
        self.assertFalse(complete_pairs_for_pose(*fixture(presentation=0),HEAD))
        self.assertFalse(complete_pairs_for_pose(*fixture(eyes=(0,)),HEAD))

    def test_changed_pose_does_not_match_held_baseline(self):
        game,host=fixture()
        self.assertFalse(complete_pairs_for_pose(game.replace('head=0,0,0,','head=0.1,0,0,'),host,HEAD))

class DepthProbeChecks(unittest.TestCase):
    @staticmethod
    def fixture(wrong_eye=None):
        rows=[]
        for eye in (0,1,-1):
            rows.append(f'Lab world draw attempts request=3 eye={eye} aborted=0 calls=10 primitives=20 readFailures=0')
            wrong=eye==wrong_eye
            rows.append(f'Lab world draw state request=3 eye={eye} z=1 write=1 alpha=1 blend=0 calls=10 primitives=20 fullRange={10 if wrong else 0} worldRange={0 if wrong else 10} otherRange=0')
        return '\n'.join(rows)

    def test_right_eye_counterexample(self):
        self.assertTrue(first_person_depth_probe(self.fixture())['coherent'])
        report=first_person_depth_probe(self.fixture(wrong_eye=1))
        self.assertFalse(report['coherent'])
        self.assertEqual(report['wrong_ranges'],[{'request':3,'eye':1,'full_range_opaque_calls':10}])

    def test_partial_or_uncertain_observation_cannot_pass(self):
        self.assertFalse(first_person_depth_probe('')['coherent'])
        self.assertFalse(first_person_depth_probe(self.fixture().replace('eye=-1','eye=2'))['coherent'])
        for before,after in [('aborted=0','aborted=1'),('readFailures=0','readFailures=1'),('primitives=20','primitives=21')]:
            self.assertFalse(first_person_depth_probe(self.fixture().replace(before,after,1))['coherent'])
        self.assertFalse(first_person_depth_probe(self.fixture()+'\n'+self.fixture())['coherent'])

class DualTopologyChecks(unittest.TestCase):
    def test_topology_is_two_hands_and_exact_identity(self):
        row='Lab native primary topology input=20 session=2 reference=3 tracking=7 owner=9 weapons=10,11 receivers=100,110 hands=3 combo=1 dual=1 flip=0 buttons=1,0 topology_compatible=1'
        self.assertEqual(len(dual_topologies(row,(2,3,7),10,30)),1)
        for before,after in [('tracking=7','tracking=0'),('session=2','session=4'),('reference=3','reference=4'),
            ('hands=3','hands=1'),('weapons=10,11','weapons=10,10'),('receivers=100,110','receivers=100,100'),
            ('combo=1','combo=0'),('buttons=1,0','buttons=0,0'),('topology_compatible=1','topology_compatible=0')]:
            self.assertFalse(dual_topologies(row.replace(before,after),(2,3,7),10,30))

class DualPhaseChecks(unittest.TestCase):
    @staticmethod
    def sample(sequence,tick,ui,trigger=(0,0),fire=(2,3)):
        return {'input_sequence':sequence,'input_tick_ms':tick,'ui_tick_ms':ui,
            'trigger':list(trigger),'fire_sequence':list(fire),'session':2,'reference':3,
            'tracking_generation':7,'primary_generations':[8,9],
            'hand_valid':[1,1],'primary_active_mask':3}

    def test_convergence_can_wait_but_later_contradiction_fails(self):
        phase=DualPhaseEvidence(10,[0,1])
        phase.observe(self.sample(11,100,90))
        phase.observe(self.sample(12,110,100,(0,1)))
        with self.assertRaises(RuntimeError):phase.observe(self.sample(13,120,110))

    def test_repeated_inputs_or_regression_cannot_certify_progress(self):
        phase=DualPhaseEvidence(10,[0,0])
        phase.observe(self.sample(11,100,90));phase.observe(self.sample(11,100,90))
        with self.assertRaises(RuntimeError):phase.require_progress()
        phase.observe(self.sample(13,120,110))
        with self.assertRaises(RuntimeError):phase.observe(self.sample(12,110,100))

    def test_neutral_uses_post_input_ui_and_rejects_resumed_fire(self):
        phase=DualPhaseEvidence(10,[0,0])
        for n in range(3):phase.observe(self.sample(11+n,100+n,97+n))
        with self.assertRaises(RuntimeError):phase.require_stopped()
        for n in range(3):phase.observe(self.sample(14+n,110+n,105+n))
        phase.require_stopped()
        phase.observe(self.sample(17,115,109,fire=(3,3)))
        with self.assertRaises(RuntimeError):phase.require_stopped()

    def test_eye_capture_requires_confirmed_phase_and_frozen_identity(self):
        phase=DualPhaseEvidence(10,[0,1])
        first=self.sample(12,110,100,(0,1));after=self.sample(15,130,120,(0,1))
        phase.observe(first);phase.observe(after)
        metadata=self.sample(14,120,110,(0,1))
        identity=(2,3,7,8,9)
        require_dual_capture(metadata,phase,identity,after)
        for key,value in [('input_sequence',11),('input_sequence',16),('input_tick_ms',109),
            ('trigger',[1,1]),('session',4),('reference',4),('tracking_generation',8),
            ('primary_generations',[9,9]),('hand_valid',[1,0]),('primary_active_mask',1)]:
            with self.assertRaises(RuntimeError):require_dual_capture(metadata|{key:value},phase,identity,after)
        with self.assertRaises(RuntimeError):require_dual_capture(metadata,phase,identity,after|{'trigger':[0,0]})

    def test_charged_release_can_fire_then_requires_a_measured_quiet_interval(self):
        phase=DualPhaseEvidence(10,[0,0])
        phase.observe(self.sample(11,100,110,fire=(2,3)))
        phase.observe(self.sample(12,120,130,fire=(3,3)))
        for n in range(4):phase.observe(self.sample(13+n,200+n*400,210+n*400,fire=(3,3)))
        phase.require_quiet_after(100)
        with self.assertRaises(RuntimeError):phase.require_quiet_after(1000)
        phase.observe(self.sample(17,1800,1810,fire=(4,3)))
        with self.assertRaises(RuntimeError):phase.require_quiet_after(100)

    def test_zap_receipts_bind_release_controls_identity_and_native_pair(self):
        topology=[{'owner':9,'weapon':[10,11],'receiver':[100,110]}]
        row='Lab native weapon event=release input=20 session=2 reference=3 tracking=7 owner=9 weaponHint=10 receiver=100 id=1 hand=0 stateBefore=5 returned=1 aborted=0 result=0 tick=100 actions=8,9 trigger=0,1 completion=120'
        args=((2,3,7,8,9),10,30,topology,'release')
        self.assertEqual(dual_weapon_events(row,*args)[0]['trigger'],[0,1])
        for before,after in [('actions=8,9','actions=8,10'),('id=1','id=12'),('returned=1','returned=0'),
            ('aborted=0','aborted=1'),('owner=9','owner=8'),('receiver=100','receiver=110'),
            ('result=0','result=1'),('trigger=0,1','trigger=-1,1'),('completion=120','completion=99')]:
            self.assertFalse(dual_weapon_events(row.replace(before,after),*args))

    def test_fire_before_confirmed_rise_cannot_supply_new_cycle(self):
        phase=DualPhaseEvidence(10,[0,1])
        phase.observe(self.sample(11,100,90))
        phase.observe(self.sample(12,110,100,(0,1)))
        phase.observe(self.sample(13,120,110,(0,1)))
        topology=[{'owner':9,'weapon':[10,11],'receiver':[100,110]}]
        row='Lab native weapon event=fire input=11 session=2 reference=3 tracking=7 owner=9 weaponHint=11 receiver=110 id=1 hand=1 stateBefore=5 returned=1 aborted=0 result=1 tick=100 actions=8,9 trigger=0,0 completion=120'
        args=((2,3,7,8,9),phase.confirmed_boundary(),21,topology)
        self.assertFalse(successful_dual_fire(row,*args))
        self.assertEqual(successful_dual_fire(row.replace('input=11','input=12').replace('trigger=0,0','trigger=0,1'),*args),{1})

if __name__=='__main__':unittest.main()
