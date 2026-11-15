{
  description = "A very basic flake";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";
  };

  outputs = inputs: {
    packages = builtins.mapAttrs (system: pkgs: {
      btop = pkgs.btop.overrideAttrs { src = ./.; };

      default = inputs.self.packages.${system}.btop;
    }) inputs.nixpkgs.legacyPackages;
  };
}
