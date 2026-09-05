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
  # Phase 13: the native RGBA->JPEG/WebP re-encoder can link libwebp for its
  # WebP path. Whether that produces a *dynamic* dependency depends on how
  # the decoder in Libraries/ was built: a local dev build against Homebrew's
  # webp is dynamically linked and needs libwebp.7/libwebpmux.3/
  # libwebpdemux.2/libsharpyuv.0 embedded alongside it (R3-T1b, 2026-09-05 --
  # this app shipped without them once already when the list below was a
  # fixed, hand-maintained enumeration that did not name them); the
  # release/CI build links libwebp STATICALLY (native/cmake/encode.cmake),
  # so those four files never exist next to the decoder at all in that
  # layout. A fixed list can only ever be right for one of these two cases -
  # this bit it twice, once in each direction, which is why 'Libraries/*'
  # (a glob, resolved by CocoaPods against whatever is actually staged in
  # this directory at pod-install time, exactly like source_files above)
  # replaces the enumeration: it embeds all six files in the release/CI
  # layout and all ten in a local dynamic-webp dev layout, with no edit
  # required when either set changes.
  s.vendored_libraries = 'Libraries/*'
end
