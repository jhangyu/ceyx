#
# Packaging-only pod: it carries the prebuilt dng_decoder_native dylib into the
# host app bundle. There is no plugin class — `ffiPlugin: true` in pubspec.yaml
# means Flutter does not generate any registration for it.
#
Pod::Spec.new do |s|
  s.name             = 'ceyx'
  s.version          = '0.1.0'
  s.summary          = 'Prebuilt dng_decoder_native library for host apps.'
  s.description      = <<-DESC
Bundles libdng_decoder_native.dylib into the host app's Frameworks directory so
package:ceyx_example can dlopen it without a dev-machine CMake build tree.
                       DESC
  s.homepage         = 'https://github.com/jhangyu/ceyx'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'ceyx' => 'noreply@example.com' }
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'

  s.platform = :osx, '11.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }

  # libdng_decoder_native.dylib links liblcms2/libjpeg (LibRaw's colour
  # management + JPEG deps) via find_package(), which resolves to Homebrew on
  # the build machine. native/cmake/pipeline.cmake's POST_BUILD step vendors
  # those two next to the dylib and repoints all three to @rpath/<name>, so
  # once CocoaPods embeds all three in Frameworks/ the loader resolves them
  # without requiring Homebrew on the host machine.
  #
  # Phase 2 (HEIC): libheif/libde265 are built by
  # native/scripts/build_deps.py build heif-stack with CMAKE_INSTALL_NAME_DIR=@rpath, so
  # they already carry @rpath install names and need no rewriting -- they are
  # simply staged next to the decoder dylib by native/cmake/heif.cmake and
  # embedded here. They are DYNAMICALLY linked on purpose: both are
  # LGPL-3.0-or-later, and a replaceable .dylib in Frameworks/ satisfies
  # section 4(d)(1) with no obligation to ship relinkable object files.
  #
  # libomp: when native/third_party/libomp/lib/libomp.dylib is present, the
  # OpenMP resolution in native/cmake/tests.cmake prefers that VENDORED copy
  # over a Homebrew one, restamps it to @rpath/libomp.dylib and stages it next
  # to the decoder. The decoder therefore carries an @rpath/libomp.dylib load
  # command, so libomp must be embedded too -- otherwise dyld fails at launch
  # on any machine without Homebrew's libomp, which is precisely the
  # prerequisite-free bundle this directory exists to produce.
  #
  # Phase 13: the native RGBA->JPEG/WebP re-encoder linked libwebp for its
  # WebP path. libwebp.7 depends on libsharpyuv.0; libwebpmux.3 and
  # libwebpdemux.2 both depend on libwebp.7 and libsharpyuv.0. All four are
  # staged next to the decoder the same way as the other vendored libraries
  # and must be embedded here for the same reason: without them dyld fails to
  # load the decoder at all on a host with no Homebrew webp installed
  # (R3-T1b, 2026-09-05 -- this app shipped without them once already).
  s.vendored_libraries = 'Libraries/libdng_decoder_native.dylib',
                         'Libraries/liblcms2.2.dylib',
                         'Libraries/libjpeg.8.dylib',
                         'Libraries/libheif.1.dylib',
                         'Libraries/libde265.0.dylib',
                         'Libraries/libomp.dylib',
                         'Libraries/libwebpmux.3.dylib',
                         'Libraries/libwebpdemux.2.dylib',
                         'Libraries/libwebp.7.dylib',
                         'Libraries/libsharpyuv.0.dylib'
end
