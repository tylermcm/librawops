"""Original Cube fixtures, file ownership, legacy bounds and saved integration."""
import array
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,sys.argv.pop(1))
import rawengine_native as raw

def numbers(result):
    out=array.array('f');out.frombytes(result[2]);return out

def cube3(n=2):
    return 'TITLE "original affine # fixture"\nLUT_3D_SIZE '+str(n)+'\n'+''.join(
        f'{r+.25*g:.17g} {g-.125*b:.17g} {b:.17g}\n'
        for b in (i/(n-1) for i in range(n)) for g in (i/(n-1) for i in range(n)) for r in (i/(n-1) for i in range(n)))

def cube1(n=2):
    return 'LUT_1D_SIZE '+str(n)+'\n'+''.join(f'{2*x-.25:.17g} {x:.17g} {-x+.5:.17g}\n' for x in (i/(n-1) for i in range(n)))

def source(session):
    doc=json.loads(session.export_manifest());doc['operations']=[];doc['output']=doc['sources'][0]['id'];return doc

def add(base,parsed):
    doc=copy.deepcopy(base)
    op={k:copy.deepcopy(parsed[k]) for k in ('type','schema_version','processing_version','input_domain','output_domain','parameters')}
    op.update(id='93000000-0000-0000-0000-000000000090',enabled=True,inputs={'image':doc['output']},masks={},blend_mode='normal',opacity=1)
    doc['operations'].append(op);doc['output']=op['id'];return doc

class CubeTests(unittest.TestCase):
    def test_size_selection_layout_domain_and_affine_truth(self):
        data=array.array('f',[-1,.25,2,.75,-.5,.25,2,1,-1])
        for space in ('prophoto-d50','rec2020-d65'):
            session=raw.RasterSession(data,3,1,space)
            try:
                base=source(session)
                for dimension,size in ((1,2),(1,256),(1,257),(1,4096),(3,2),(3,17),(3,18),(3,33)):
                    parsed=raw.parse_cube_lut(cube1(size) if dimension==1 else cube3(size),working_space=space)
                    self.assertEqual(parsed['type'],f'rawengine.lut{dimension}d'+('_large' if size>(256 if dimension==1 else 17) else ''))
                    text=json.dumps(add(base,parsed));result=session.render_manifest(text)
                    actual=numbers(result)
                    for i in range(0,len(data),3):
                        r,g,b=data[i:i+3];expected=(2*r-.25,g,-b+.5) if dimension==1 else (r+.25*g,g-.125*b,b)
                        for c in range(3):self.assertAlmostEqual(actual[i+c],expected[c],delta=4e-7)
                    self.assertEqual(session.render_manifest(text,{'tile_size':1}),result)
                    self.assertEqual(session.required_source_regions(text),session.required_source_regions(json.dumps(base)))
            finally:session.close()

    def test_legacy_limits_still_reject_large_tables_even_disabled(self):
        session=raw.RasterSession(array.array('f',[.25,.5,.75]),1,1,'prophoto-d50')
        try:
            for text in (cube1(257),cube3(18)):
                parsed=raw.parse_cube_lut(text,working_space='prophoto-d50');parsed['type']=parsed['type'].replace('_large','')
                for enabled in (True,False):
                    doc=add(source(session),parsed);doc['operations'][-1]['enabled']=enabled
                    with self.assertRaises(ValueError):session.render_manifest(json.dumps(doc))
        finally:session.close()

    def test_file_copy_unicode_mutation_history_jobs_and_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'naïve文件.cube';path.write_text(cube3(18),encoding='ascii')
            parsed=raw.load_cube_lut(path,working_space='rec2020-d65')
            self.assertEqual(parsed,raw.parse_cube_lut(cube3(18),working_space='rec2020-d65'))
            session=raw.RasterSession(array.array('f',[.25,.5,.75]*9),3,3,'rec2020-d65')
            try:
                base=source(session);text=json.dumps(add(base,parsed));result=session.render_manifest(text)
                before=session.cache_stats();same=raw.parse_cube_lut('# different comment\n'+cube3(18),working_space='rec2020-d65')
                self.assertEqual(session.render_manifest(json.dumps(add(base,same))),result)
                self.assertEqual(session.cache_stats()['misses'],before['misses'])
                path.write_text('invalid now',encoding='ascii');path.unlink()
                self.assertEqual(session.submit_manifest(text).result(timeout=5),result)
                self.assertEqual(session.submit_manifest_latest('view',text).result(timeout=5),result)
                history=session.history(text);saved=history.save();parsed['parameters']['values'][0]=999
                restored=session.restore_history(saved);self.assertEqual(restored.render(),result)
                self.assertEqual(session.histogram_manifest(text)['descriptor']['primaries'],'rec2020')
                tiles=[];session.analyze_local_manifest(text,tiles.append,radius=1);self.assertTrue(tiles)
                restored.close();history.close()
            finally:session.close()
            with self.assertRaises(RuntimeError):raw.load_cube_lut(path,working_space='rec2020-d65')

    def test_numeric_title_crlf_and_replay(self):
        text=' \t# fixture\r\nTITLE "numeric # fixture"\r\nDOMAIN_MIN -1 -1 -1\r\nDOMAIN_MAX +1.0 1e0 .1E+1\r\nLUT_1D_SIZE 2\r\n-1 -1 -1\r\n1 1 1\r\n'
        p=raw.parse_cube_lut(text,working_space='prophoto-d50')
        self.assertEqual(p['title'],'numeric # fixture');self.assertEqual(p['parameters']['input_min'],-1)
        self.assertEqual(p['parameters']['channels'],[[-1,1]]*3)
        session=raw.RasterSession(array.array('f',[-0.,0.,-0.,3.4028234663852886e38,-3.4028234663852886e38,2**-149]),2,1,'prophoto-d50')
        try:
            base=source(session);self.assertEqual(session.render_manifest(json.dumps(add(base,p))),session.render_manifest(json.dumps(base)))
        finally:session.close()

    def test_strict_malformed_size_bytes_rows_keywords_and_domains(self):
        invalid=['','LUT_1D_SIZE 1\n','LUT_1D_SIZE 4097\n','LUT_3D_SIZE 34\n','LUT_1D_SIZE 2.0\n',
            'LUT_1D_SIZE 2\n0 0 0\n','LUT_1D_SIZE 2\n0 0 0\n1 1 1\n2 2 2\n',
            'LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n','LUT_1D_SIZE 2\nLUT_1D_SIZE 2\n',
            'TITLE bad\n'+cube1(),'TITLE "a" "b"\n'+cube1(),
            'DOMAIN_MIN 0 1 0\n'+cube1(),'DOMAIN_MAX 0 0 0\n'+cube1(),
            'DOMAIN_MIN 0 0\n'+cube1(),'LUT_1D_INPUT_RANGE 0 1\n'+cube1(),
            '\ufeff'+cube1(),cube1().replace('\n','\r'),'é\n'+cube1(),
            '#'+('x'*250)+'\n'+cube1(),'LUT_1D_SIZE 2\n0 0 0\nDOMAIN_MAX 1 1 1\n1 1 1\n',
            'LUT_1D_SIZE 2\n0 0 0\n1 1 1 # inline\n',' '*((4*1024*1024)+1)]
        for value in ('nan','inf','-inf','0x1p0','1e','1e-999','65537','1.2.3','true'):
            invalid.append('LUT_1D_SIZE 2\n0 0 0\n'+value+' 1 1\n')
        for text in invalid:
            with self.subTest(text=text[:60]):
                with self.assertRaises(ValueError):raw.parse_cube_lut(text,working_space='prophoto-d50')
        for space in ('srgb','camera-linear','encoded-srgb'):
            with self.assertRaises(ValueError):raw.parse_cube_lut(cube1(),working_space=space)
        with self.assertRaises(TypeError):raw.parse_cube_lut(cube1())
        with self.assertRaises(TypeError):raw.parse_cube_lut(cube1().encode(),working_space='prophoto-d50')
        with self.assertRaises(ValueError):raw.load_cube_lut('bad\0path',working_space='prophoto-d50')
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'oversize.cube';path.write_bytes(b' '*(4*1024*1024+1))
            with self.assertRaises(ValueError):raw.load_cube_lut(path,working_space='prophoto-d50')

if __name__=='__main__':unittest.main()
