import unittest
import numpy as np
from image_ops import alignment,stitch,require_axes
class ImageStitchTests(unittest.TestCase):
    def test_axis_invariant(self):
        with self.assertRaises(ValueError):require_axes(False,False)
    def test_two_axis_and_duplicate_alignment(self):
        try:import cv2
        except ImportError:self.skipTest('OpenCV build dependency is not installed')
        rng=np.random.default_rng(10);source=rng.integers(0,256,(620,760,3),dtype=np.uint8)
        a=source[:500,:600];b=source[70:570,90:690]
        self.assertEqual(alignment(a,b,True,True),(90,70))
        result,positions=stitch([a,a,b],True,True)
        self.assertEqual(positions,[(0,0),(90,70)])
        self.assertEqual(result.shape,(570,690,3))
        np.testing.assert_array_equal(result[70:570,90:690],b)
    def test_unrelated_frames_fail_without_fabricated_content(self):
        try:import cv2
        except ImportError:self.skipTest('OpenCV build dependency is not installed')
        with self.assertRaises(ValueError):alignment(np.zeros((300,300,3),np.uint8),np.full((300,300,3),255,np.uint8))
    def test_pixel_budget(self):
        with self.assertRaises(ValueError):stitch([np.zeros((10,10,3),np.uint8)],max_pixels=80)
if __name__=='__main__':unittest.main()
