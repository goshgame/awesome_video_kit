#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint awesome_video_kit.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'awesome_video_kit'
  s.version          = '0.0.1'
  s.summary          = 'Flutter wrapper for AwesomeVideoKitSDK.'
  s.description      = <<-DESC
Flutter plugin that exposes AwesomeVideoKitSDK features on iOS.
                       DESC
  s.homepage         = 'https://gosh.live'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Gosh' => 'dev@gosh.live' }
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'
  s.platform         = :ios, '12.0'
  s.swift_version    = '5.0'
  s.requires_arc     = true
  # Vendored native SDK (contains FFmpeg + downloader/remuxer).
  s.vendored_frameworks = 'Framework/AwesomeVideoKitSDK.xcframework'
  s.preserve_paths      = 'Framework/AwesomeVideoKitSDK.xcframework'

  # FFmpeg commonly needs these system libs when linked statically.
  s.libraries = 'z', 'bz2', 'iconv'
  s.frameworks = 'Security'

  s.dependency 'Flutter'
  # Privacy manifest (safe to ship even if currently empty).
  s.resource_bundles = {
    'awesome_video_kit_privacy' => ['Resources/PrivacyInfo.xcprivacy']
  }
  # The bundled SDK contains arm64 slices for both device and simulator.
  s.pod_target_xcconfig = {
    'SWIFT_VERSION'                    => '5.0',
    'ENABLE_BITCODE'                   => 'NO',
    'DEFINES_MODULE'                   => 'YES',
    'BUILD_LIBRARIES_FOR_DISTRIBUTION' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386 x86_64'
  }
  s.user_target_xcconfig = {
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386 x86_64'
  }

  # If your plugin requires a privacy manifest, for example if it uses any
  # required reason APIs, update the PrivacyInfo.xcprivacy file to describe your
  # plugin's privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'awesome_video_kit_privacy' => ['Resources/PrivacyInfo.xcprivacy']}
end
