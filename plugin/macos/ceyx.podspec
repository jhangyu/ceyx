#
# Packaging-only pod: it carries the prebuilt dng_decoder_native dylib into the
# host app bundle. There is no plugin class — `ffiPlugin: true` in pubspec.yaml
# means Flutter does not generate any registration for it.
#
Pod::Spec.new do |s|
  s.name             = 'ceyx'
  s.version          = '0.0.1'
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

  # The dylib's install name is @rpath/libdng_decoder_native.dylib, so once
  # CocoaPods embeds it in Frameworks/ the loader resolves it normally.
  s.vendored_libraries = 'Libraries/libdng_decoder_native.dylib'
end
