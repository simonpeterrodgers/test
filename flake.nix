{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.05";
    nixpkgs-esp-dev.url = "github:Lindboard/nixpkgs-esp-dev";
    #"github:mirrexagon/nixpkgs-esp-dev";
  };

  outputs = {self, nixpkgs, nixpkgs-esp-dev}:
  let 
    system = "x86_64-linux";
    overlays = [ (import "${nixpkgs-esp-dev}/overlay.nix") ];
    pkgs = import nixpkgs { inherit system overlays; };
  in
  with pkgs;
  {
    devShell.${system} = mkShell {
      buildInputs = [ esp-idf-esp32c6 ];
    };
  };
}

