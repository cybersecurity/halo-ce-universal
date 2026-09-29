"""Original current-checkout build-mode tests; no game compile or network fetch."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

PATH=Path(__file__).resolve().parents[1]/'scripts/build.py'
spec=importlib.util.spec_from_file_location('portable_build',PATH)
build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class SourceCheckoutTests(unittest.TestCase):
    def make_checkout(self, directory):
        source=Path(directory)/'checkout with spaces Ω';source.mkdir()
        subprocess.run(['git','init','-q',str(source)],check=True)
        for name in ('port/macos/CMakeLists.txt','tools/macos_aot_build.py','validation/sourceport-lp64/batch_compile_guest.py','source/current.c','source/deleted.c'):
            file=source/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text('original\n')
        subprocess.run(['git','-C',str(source),'add','.'],check=True)
        subprocess.run(['git','-C',str(source),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture'],check=True)
        return source

    def test_working_changes_retained_without_fetch_patch_or_overlay(self):
        with tempfile.TemporaryDirectory() as directory:
            source=self.make_checkout(directory)
            (source/'source/current.c').write_text('working change\n')
            (source/'source/deleted.c').unlink()
            (source/'port/macos/CMakeLists.txt').write_text('current native changes\n')
            dest=Path(directory)/'snapshot'
            with patch.object(build,'checkout') as fetch, patch.object(build,'run') as command, patch.object(build.shutil,'copytree') as overlay:
                provenance=build.prepare_halo_source({},dest,source)
            fetch.assert_not_called();command.assert_not_called();overlay.assert_not_called()
            self.assertEqual((dest/'source/current.c').read_text(),'working change\n')
            self.assertEqual((dest/'port/macos/CMakeLists.txt').read_text(),'current native changes\n')
            self.assertFalse((dest/'source/deleted.c').exists())
            self.assertEqual(provenance['mode'],'current-checkout')
            self.assertNotIn(str(Path(directory).resolve()),json.dumps(provenance))

    def test_indexed_new_code_included_but_assets_and_untracked_files_excluded(self):
        with tempfile.TemporaryDirectory() as directory:
            source=self.make_checkout(directory)
            (source/'source/new.c').write_text('new source\n')
            (source/'source/untracked.c').write_text('not indexed\n')
            (source/'assets').mkdir();(source/'assets/ui.map').write_bytes(b'fixture')
            subprocess.run(['git','-C',str(source),'add','source/new.c','assets/ui.map'],check=True)
            dest=Path(directory)/'snapshot';build.snapshot_checkout(source,dest)
            self.assertTrue((dest/'source/new.c').is_file())
            self.assertFalse((dest/'source/untracked.c').exists())
            self.assertFalse((dest/'assets').exists())

    def test_symlink_and_nested_destination_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source=self.make_checkout(directory)
            with self.assertRaisesRegex(ValueError,'outside'):
                build.snapshot_checkout(source,source/'output')
            (source/'source/link.c').symlink_to(source/'source/current.c')
            subprocess.run(['git','-C',str(source),'add','source/link.c'],check=True)
            with self.assertRaisesRegex(ValueError,'regular'):
                build.snapshot_checkout(source,Path(directory)/'snapshot')

    def test_default_mode_fetches_patch_and_overlay(self):
        pins={'halo':{'url':'https://example.invalid/upstream.git','commit':'abc'}}
        with patch.object(build,'checkout') as fetch, patch.object(build,'run') as command, patch.object(build.shutil,'copytree') as overlay:
            provenance=build.prepare_halo_source(pins,Path('output'))
        fetch.assert_called_once_with(pins['halo'],Path('output'))
        command.assert_called_once()
        self.assertEqual(command.call_args.args[:2],('git','apply'))
        overlay.assert_called_once()
        self.assertEqual(provenance['mode'],'pinned-upstream')


if __name__=='__main__':unittest.main()
