{ pkgs, ... }:

{
  android = {
    enable = true;
    platforms.version = [ "28" "24" ]; # Use 28 for arm64 and 24 for armv7
    systemImageTypes = [ "google_apis_playstore" ];
    abis = [ "arm64-v8a" "armeabi-v7a" ];  # Set armv7 ABI for API level 24
    cmake.version = [ "3.22.1" ];
    cmdLineTools.version = "11.0";
    tools.version = "26.1.1";
    platformTools.version = "34.0.5";
    buildTools.version = [ "30.0.3" ];
    emulator = {
      enable = true;
      version = "34.1.9";
    };
    sources.enable = false;
    systemImages.enable = true;
    ndk.enable = true;
    googleAPIs.enable = true;
    googleTVAddOns.enable = true;
    extras = [ "extras;google;gcm" ];
    extraLicenses = [
      "android-sdk-preview-license"
      "android-googletv-license"
      "android-sdk-arm-dbt-license"
      "google-gdk-license"
      "intel-android-extra-license"
      "intel-android-sysimage-license"
      "mips-android-sysimage-license"
    ];
    android-studio = {
      enable = true;
      package = pkgs.android-studio;
    };
  };

  packages = [ pkgs.hello ];

  scripts.say-bye.exec = ''
    echo bye
  '';
}
