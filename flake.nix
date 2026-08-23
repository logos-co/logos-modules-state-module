{
  description = "modules_state - read-only registry of module lifecycle state";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
    in
    module // {
      # Written as a literal `checks =` on purpose: `ws sync-graph` decides
      # hasTests by grepping the flake for exactly that, so a checks output
      # reached any other way records hasTests=false and `ws test` then reports
      # "no tests" WITHOUT failing — which is the same green-by-absence this
      # module's own invariants are here to stop.
      checks = logos-module-builder.lib.mkLogosModuleTests {
        src = ./.;
        testDir = ./tests;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
    };
}
