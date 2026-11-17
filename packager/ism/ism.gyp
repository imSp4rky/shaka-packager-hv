{
  'variables': {'shaka_code': 1},
  'targets': [{
    'target_name': 'ism',
    'type': '<(component)',
    'sources': ['ism_manifest.cc', 'ism_manifest.h', 'ism_input.cc', 'ism_input.h',
                'ism_fragment.cc'],
    'dependencies': [
      '../file/file.gyp:file',
      '../media/base/media_base.gyp:media_base',
      '../media/codecs/codecs.gyp:codecs',
      '../media/formats/mp4/mp4.gyp:mp4',
      '../third_party/libxml/libxml.gyp:libxml',
    ],
  }, {
    'target_name': 'ism_unittest',
    'type': '<(gtest_target_type)',
    'sources': ['ism_manifest_unittest.cc', 'ism_input_unittest.cc',
                'ism_fragment_unittest.cc'],
    'dependencies': ['ism', '../testing/gtest.gyp:gtest'],
  }],
}
