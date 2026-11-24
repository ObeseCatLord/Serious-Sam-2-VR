package main

import "testing"

func TestMergeLaunchOptions(t *testing.T) {
	cases := []struct {
		in   string
		arm  bool
		want string
	}{
		{"", false, `WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`},
		{"", true, `STEAM_COMPAT_FEX_CONFIG=TSOEnabled:0,Multiblock:1 WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`},
		{"-nosound", false, `WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command% -nosound`},
		{`WINEDLLOVERRIDES="dinput8=n,b" %command%`, false, `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 WINEDLLOVERRIDES="d3d9=n,b;dinput8=n,b" %command%`},
		{`gamemoderun %command%`, false, `WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 gamemoderun %command%`},
		{`STEAM_COMPAT_FEX_CONFIG=TSOEnabled:1 WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`, true,
			`STEAM_COMPAT_FEX_CONFIG=TSOEnabled:1 WINEDLLOVERRIDES="d3d9=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`},
	}
	for _, c := range cases {
		if got := mergeLaunchOptions(c.in, c.arm); got != c.want {
			t.Errorf("merge(%q, %v) = %q, want %q", c.in, c.arm, got, c.want)
		}
		if again := mergeLaunchOptions(c.want, c.arm); again != c.want {
			t.Errorf("merge is not idempotent for %q: %q", c.want, again)
		}
	}
}

func TestVdfLaunchOptions(t *testing.T) {
	text := "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n\t\t\t\t\"apps\"\n\t\t\t\t{\n" +
		"\t\t\t\t\t\"204340\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LastPlayed\"\t\t\"1\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n" +
		"\t\"path\"\t\t\"C:\\\\a \\\"b\\\"\"\n}\n"
	tree, err := vdfParse(text)
	if err != nil {
		t.Fatal(err)
	}
	if vdfWrite(tree) != text {
		t.Fatalf("round trip changed the file:\n%s", vdfWrite(tree))
	}
	app := tree.path(false, "UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "204340")
	if app == nil {
		t.Fatal("app block not found")
	}
	want := mergeLaunchOptions("", true)
	app.set("LaunchOptions", vdfEscape(want))
	again, err := vdfParse(vdfWrite(tree))
	if err != nil {
		t.Fatal(err)
	}
	leaf, ok := again.path(false, "UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "204340").leaf("LaunchOptions")
	if !ok || vdfUnescape(leaf.value) != want {
		t.Fatalf("launch options read back as %q", leaf.value)
	}
}
