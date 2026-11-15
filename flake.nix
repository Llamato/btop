{
  description = "btop with the Asahi AGX GPU collector (DRM_IOCTL_ASAHI_GET_STATS)";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";
  };

  outputs = inputs: {
    packages = builtins.mapAttrs (system: pkgs: {
      # upstream layout: bin/btop
      btop = pkgs.btop.overrideAttrs {
        src = ./.;
      };

      # same build under its own name, so it can be installed next to the distro btop.
      # On aarch64 this build has the Asahi collector compiled in (BTOP_GPU / GPU_SUPPORT); it is a
      # no-op on machines without the asahi DRM driver.
      btop-asahi = pkgs.btop.overrideAttrs (final: prev: {
        pname = "btop-asahi";
        src = ./.;
        postInstall = (prev.postInstall or "") + ''
          mv "$out/bin/btop" "$out/bin/btop-asahi"
        '';
        meta = prev.meta // {
          mainProgram = "btop-asahi"; # versionCheckHook looks up bin/<mainProgram>
          description = (prev.meta.description or "resource monitor") + " (Asahi AGX GPU stats)";
        };
      });

      default = inputs.self.packages.${system}.btop;
    }) inputs.nixpkgs.legacyPackages;
  };
}
