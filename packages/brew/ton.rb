class Ton < Formula
  desc "A collection of The Open Network core software and utilities."
  homepage "http://github.com/ton-blockchain/ton"

  if Hardware::CPU.arm?
    head "https://tonthemoon.github.io/ton-repo/brew/ton-arm64.tar.gz"
  else
    head "https://tonthemoon.github.io/ton-repo/brew/ton-x86_64.tar.gz"
  end
  license "LGPL-2.0-only"

  def install
    bin.install "bin/fift"
  end
end
