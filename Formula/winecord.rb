class Winecord < Formula
  desc "Discord Rich Presence bridge for Windows games running through Wine on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://raw.githubusercontent.com/Zard-Studios/homebrew-tap/main/releases/winecord-0.1.7-macos-universal.tar.gz"
  sha256 "dddac3b0c2596a899f69c9f8feb420aa97588187d5f66a694cb2d8753032092d"
  license "MIT"

  def install
    bin.install "bin/winecord"
    libexec.install "libexec/winecord"
    doc.install "README.md"
    prefix.install "LICENSE", "NOTICE"
  end

  def caveats
    <<~EOS
      Finish setup with:
        winecord setup

      Remove WineCord's LaunchAgent and Wine-side helper before uninstalling:
        winecord uninstall
        brew uninstall winecord

      If a configured Wine prefix is on an external volume, connect it before running winecord uninstall.

      If your Wine prefix is not auto-detected:
        winecord setup --prefix /path/to/prefix

      If that prefix needs a specific Wine runner:
        winecord setup --prefix /path/to/prefix --wine /path/to/wine
    EOS
  end

  test do
    assert_match "WineCord", shell_output("#{bin}/winecord --version")
  end
end
