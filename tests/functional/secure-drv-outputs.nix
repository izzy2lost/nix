with import "${builtins.getEnv "_NIX_TEST_BUILD_DIR"}/config.nix";

{

  good = mkDerivation {
    name = "good";
    builder = builtins.toFile "builder" ''
      mkdir $out
      echo > $out/good
    '';
  };

  bad = mkDerivation {
    name = "good";
    builder = builtins.toFile "builder" ''
      mkdir $out
      echo > $out/bad
    '';
  };

}
