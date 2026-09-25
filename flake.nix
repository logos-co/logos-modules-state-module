{
  description = "modules_state - read-only registry of module lifecycle state";

  inputs = {
    # On the in-process builder (logos-module-builder#261) until it and the chain under it merge.
    logos-module-builder.url = "github:logos-co/logos-module-builder/feat/inproc-eligible-plain";
    # Cut the builder -> standalone-app -> liblogos -> this-module cycle, as
    # logos-capability-module does. Safe only because this is a `core` module:
    # mkLogosModule forces the input for type "ui" alone. Never copy to ui/ui_qml.
    logos-module-builder.inputs.logos-standalone-app.follows = "";
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
