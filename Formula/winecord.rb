class Winecord < Formula
  desc "Discord Rich Presence bridge for Windows games running through Wine on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://raw.githubusercontent.com/Zard-Studios/homebrew-tap/main/releases/winecord-0.1.9-macos-universal.tar.gz"
  sha256 "fe5bd2c4df34a625deadbfc05aceb8e2d5e4dd362f4a2e8137a0fd1acaa91ea6"
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

      Update WineCord and refresh the installed helper with:
        winecord update

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
