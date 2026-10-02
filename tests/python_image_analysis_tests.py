"""Independent histogram oracle, saved graph taps, ownership and level coverage."""
import array
import bisect
import json
import math
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
import rawengine_native as raw


def source_manifest(session):
    doc = json.loads(session.export_manifest())
    doc.update(operations=[], output=doc["sources"][0]["id"])
    return json.dumps(doc)


def expected_channel(values, bins, lower, upper):
    # Explicit interior edges and binary search independently of native scaling.
    edges = [lower + (upper - lower) * i / bins for i in range(1, bins)]
    counts = [0] * bins
    finite = [v for v in values if math.isfinite(v)]
    for v in finite:
        if lower <= v <= upper:
            counts[bisect.bisect_right(edges, v)] += 1
    return dict(counts=counts, underflow=sum(v < lower for v in finite),
                overflow=sum(v > upper for v in finite), nonfinite=len(values) - len(finite),
                minimum=min(finite) if finite else None, maximum=max(finite) if finite else None)


class AnalysisTests(unittest.TestCase):
    def assert_oracle(self, session, manifest, request=None, bins=16, lower=-0.5, upper=1.5):
        rendered = session.render_manifest(manifest, request)
        values = array.array("f"); values.frombytes(rendered[2])
        result = session.histogram_manifest(manifest, request, bins=bins, lower=lower, upper=upper)
        self.assertEqual(result["pixel_count"], rendered[0] * rendered[1])
        self.assertEqual(result["bins"], bins)
        self.assertEqual((result["lower"], result["upper"]), (lower, upper))
        for c, channel in enumerate(result["channels"]):
            self.assertEqual(channel, expected_channel(values[c::3], bins, lower, upper))
            self.assertEqual(sum(channel["counts"]) + channel["underflow"] + channel["overflow"] + channel["nonfinite"],
                             result["pixel_count"])
        return result

    def test_bin_boundaries_and_copy_ownership(self):
        pixels = array.array("f", [v for v in (-0.5,-0.25,0,0.25,0.5,0.75,1,1.25,1.5) for _ in range(3)])
        original = pixels.tobytes()
        session = raw.RasterSession(pixels,9,1,"prophoto-d50")
        manifest = source_manifest(session)
        result = self.assert_oracle(session,manifest,bins=4,lower=0,upper=1)
        self.assertEqual(result["channels"][0]["counts"], [1,1,1,2])
        self.assertEqual(result["channels"][0]["underflow"],2)
        self.assertEqual(result["channels"][0]["overflow"],2)
        self.assertEqual(result["bounds"], (0,0,9,1))
        self.assertEqual(result["descriptor"], dict(format="rgb_float32",domain="scene_linear_rgb",primaries="prophoto",
                         white_point="d50",transfer="linear",reference="scene_referred",alpha="none",profile_sha256="0"*64))
        result["channels"][0]["counts"][0] = 999
        result["descriptor"]["primaries"] = "edited"
        pixels[0] = 7
        self.assertEqual(session.render_manifest(manifest)[2],original)
        self.assertEqual(self.assert_oracle(session,manifest,bins=4,lower=0,upper=1)["channels"][0]["counts"],[1,1,1,2])
        session.close()

    def test_geometry_levels_spaces_tiles_and_cache(self):
        pixels = array.array("f", ((i*37 % 257 - 40)/128 for i in range(11*9*3)))
        for space, primaries, white in (("prophoto-d50","prophoto","d50"),("rec2020-d65","rec2020","d65")):
            session = raw.RasterSession(pixels,11,9,space)
            source = source_manifest(session)
            display = session.export_manifest(dict(output_mode="srgb-preview",crop=(1,1,9,7),rotate=90,
                                                   resize=(9,5),resize_filter="area",exposure_stops=0.5))
            for manifest in (source,display):
                for mip in (0,1,2):
                    reference = None
                    for tile_size in (1,3,256):
                        request = dict(mip=mip, quality="preview" if mip else "final",tile_size=tile_size)
                        result = self.assert_oracle(session,manifest,request)
                        if reference is None: reference = result
                        self.assertEqual(result,reference)
                    request.update(x=1,y=1,roi_width=1,roi_height=1)
                    self.assert_oracle(session,manifest,request)
            d = session.histogram_manifest(source)["descriptor"]
            self.assertEqual((d["primaries"],d["white_point"]),(primaries,white))
            self.assertEqual(session.histogram_manifest(display)["descriptor"]["domain"],"display_encoded_rgb")
            self.assertEqual(session.histogram_manifest(display)["descriptor"]["transfer"],"srgb")
            session.clear_cache()
            first = session.histogram_manifest(display, {"tile_size":3})
            stats = session.cache_stats()
            self.assertEqual(first,session.histogram_manifest(display,{"tile_size":3}))
            self.assertEqual(stats["misses"],session.cache_stats()["misses"])
            self.assertGreater(session.cache_stats()["hits"],stats["hits"])
            self.assertEqual(session.export_manifest(dict(output_mode="srgb-preview",crop=(1,1,9,7),rotate=90,
                              resize=(9,5),resize_filter="area",exposure_stops=0.5)),display)
            session.close()

    def test_raw_source_calibration_and_nonzero_sensor_roi(self):
        samples = array.array("H", ((x*1931+y*2731)%45000 for y in range(9) for x in range(11)))
        metadata = dict(active_x=1,active_y=1,active_width=9,active_height=7,black_level=1000,white_level=30000)
        # Metadata uses the four-site form accepted by owned RAW sessions.
        metadata.pop("black_level"); metadata.pop("white_level")
        metadata.update(black_levels=(1000,)*4,white_levels=(30000,)*4)
        for algorithm in ("rawengine.bilinear","rawengine.menon_base"):
            session = raw.RawSession(samples,11,9,metadata,demosaic=dict(algorithm=algorithm,processing_version=1))
            source = source_manifest(session)
            result = self.assert_oracle(session,source,{"tile_size":2})
            self.assertEqual(result["bounds"],(1,1,9,7))
            self.assertEqual(result["descriptor"]["domain"],"camera_linear_rgb")
            self.assertEqual(result,self.assert_oracle(session,source,{"tile_size":256}))
            calibrated = json.loads(session.export_manifest(dict(red_gain=1.2,blue_gain=1.7,
                camera_to_xyz_d50=(0.6,0.2,0.1,0.2,0.6,0.1,0.1,0.1,0.6),output_mode="srgb-preview")))
            tap = next(op for op in calibrated["operations"] if op["type"] == "rawengine.camera_to_working")
            calibrated["output"] = tap["id"]
            text = json.dumps(calibrated)
            for mip in (0,1,2):
                req = dict(mip=mip,quality="preview" if mip else "final",tile_size=2)
                result = self.assert_oracle(session,text,req)
                self.assertEqual(result["descriptor"]["domain"],"scene_linear_rgb")
                self.assertEqual(result,self.assert_oracle(session,text,{**req,"tile_size":256}))
            session.close()

    def test_multisource_output(self):
        a,b = "10000000-0000-0000-0000-000000000001","10000000-0000-0000-0000-000000000002"
        spec = dict(rgb=array.array("f",[0.25,0.5,0.75]*35),width=7,height=5,working_space="prophoto-d50")
        other = {**spec,"rgb":array.array("f",[-0.25,1,2]*35)}
        session = raw.RasterGraphSession({a:spec,b:other})
        doc = json.loads(session.export_manifest(a))
        op = dict(id="10000000-0000-0000-0000-000000000010",type="rawengine.linear_mix",schema_version=1,
                  processing_version=2,enabled=True,input_domain="scene_linear_prophoto_d50",output_domain="scene_linear_prophoto_d50",
                  inputs={"base":a,"layer":b},parameters={"amount":0.5},masks={},blend_mode="normal",opacity=1)
        doc.update(operations=[op],output=op["id"])
        for mip in (0,1,2):
            self.assert_oracle(session,json.dumps(doc),dict(mip=mip,quality="preview" if mip else "final",tile_size=2))
        session.close()

    def test_validation_and_closed_session(self):
        session = raw.RasterSession(array.array("f",[0.25,0.5,0.75]),1,1,"prophoto-d50")
        text = source_manifest(session)
        for args in (dict(bins=0),dict(bins=65537),dict(bins=-1),dict(lower=1,upper=1),dict(lower=2),
                     dict(lower=float("nan")),dict(upper=float("inf")),dict(lower=-1e308,upper=1e308)):
            with self.assertRaises((ValueError,OverflowError)): session.histogram_manifest(text,**args)
        for bins in (True,4.0,"4"):
            with self.assertRaises(TypeError): session.histogram_manifest(text,bins=bins)
        with self.assertRaises(TypeError): session.histogram_manifest(text,[])
        with self.assertRaises(TypeError): session.histogram_manifest(text,None,4)
        for request in ({"exposure_stops":1},{"bins":4},{"tile_size":0},{"roi_width":0},{"mip":1,"quality":"final"}):
            with self.assertRaises(ValueError): session.histogram_manifest(text,request)
        with self.assertRaises(ValueError): session.histogram_manifest("{}")
        self.assert_oracle(session,text,bins=65536)
        self.assert_oracle(session,text,bins=1)
        session.close()
        with self.assertRaises(RuntimeError): session.histogram_manifest(text)


if __name__ == "__main__": unittest.main()
