// Pak reader behind `just data` (scripts/data.py). Prints to stdout; no cache logic here.
//   key <exe> <pak>  find the pak AES key in the shipping exe (prints 0x… hex; caller stores it outside the repo)
//   paths            every file path in the paks
//   registry         "<object path>\t<class>" from AssetRegistry.bin
//   export <out> <p>… one JSON file per package under <out>/<p>.json
//   script <out> <p>… same, only the exports that carry Blueprint logic (functions, classes, delegate bindings)
// Env: PAKS (Content/Paks dir), AES_KEY_FILE (0x… hex), OODLE_DIR (where the Oodle lib is cached).
using System.Security.Cryptography;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.AssetRegistry;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using Newtonsoft.Json;

if (args.FirstOrDefault() == "key") return Key.Find(args[1], args[2]);

string Env(string k) => Environment.GetEnvironmentVariable(k) ?? throw new Exception($"{k} not set");
var oodle = Path.Combine(Env("OODLE_DIR"), OodleHelper.OodleFileName);
if (!File.Exists(oodle)) { string? o = oodle; OodleHelper.DownloadOodleDll(ref o); }
OodleHelper.Initialize(oodle);

var p = new DefaultFileProvider(Env("PAKS"), SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE4_27), StringComparer.OrdinalIgnoreCase)
{ ReadScriptData = true };
p.Initialize();
p.SubmitKey(new FGuid(), new FAesKey(File.ReadAllText(Env("AES_KEY_FILE")).Trim()));

var stdout = Console.Out;
switch (args.FirstOrDefault())
{
    case "paths":
        foreach (var f in p.Files.Keys.Order()) stdout.WriteLine(f);
        break;
    case "registry":
        var reg = p.Files.Keys.First(k => k.EndsWith("AssetRegistry.bin", StringComparison.OrdinalIgnoreCase));
        foreach (var a in new FAssetRegistryState(p.CreateReader(reg)).PreallocatedAssetDataBuffers)
            stdout.WriteLine($"{a.ObjectPath}\t{a.AssetClass}");
        break;
    case "export" or "script":
        var fail = 0;
        foreach (var path in args.Skip(2))
        {
            var dst = Path.Combine(args[1], path + ".json");
            try
            {
                var pkg = p.LoadPackage(path);
                var json = JsonConvert.SerializeObject(args[0] == "export" ? pkg.GetExports() : ((Package)pkg).ExportMap
                    .Select((x, i) => (x.ClassName, i)).Where(x => x.ClassName is "Function" or "DelegateFunction" || x.ClassName.EndsWith("GeneratedClass")
                        || x.ClassName.EndsWith("DelegateBinding")).Select(x => pkg.ExportsLazy[x.i].Value), Formatting.Indented);
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                File.WriteAllText(dst, json);
                stdout.WriteLine(dst);
            }
            catch (Exception e) { fail++; Console.Error.WriteLine($"!! {path}: {e.Message}"); }
        }
        return fail == 0 ? 0 : 1;
    default:
        Console.Error.WriteLine("usage: key <exe> <pak> | paths | registry | export|script <outdir> <package path>…");
        return 2;
}
return 0;

static class Key
{
    // UE writes the key with 8 consecutive `mov dword [reg+disp], imm32` (C7 /0). Every run of 8 is a candidate;
    // the right one decrypts the pak index to its mount point FString ("../../../").
    public static int Find(string exePath, string pakPath)
    {
        var exe = File.ReadAllBytes(exePath);
        using var pak = File.OpenRead(pakPath);
        var tail = new byte[221]; pak.Seek(-221, SeekOrigin.End); pak.ReadExactly(tail);  // FPakInfo v11
        var block = new byte[16]; pak.Seek(BitConverter.ToInt64(tail, 25), SeekOrigin.Begin); pak.ReadExactly(block);
        using var aes = System.Security.Cryptography.Aes.Create();
        var run = new List<(int start, int end, int imm)>();
        for (var i = 0; i < exe.Length - 16;)
        {
            var len = MovImmLength(exe, i);
            if (len == 0) { i++; continue; }
            if (run.Count > 0 && i - run[^1].end > 12) run.Clear();
            run.Add((i, i + len, i + len - 4));
            i += len;
            if (run.Count < 8) continue;
            var key = run.TakeLast(8).SelectMany(r => exe.AsSpan(r.imm, 4).ToArray()).ToArray();
            aes.Key = key;
            var plain = aes.DecryptEcb(block, PaddingMode.None);
            var n = BitConverter.ToInt32(plain, 0);
            if (n is > 0 and < 512 && plain.AsSpan(4, 9).SequenceEqual("../../../"u8))
            {
                Console.WriteLine("0x" + Convert.ToHexString(key));
                return 0;
            }
        }
        Console.Error.WriteLine("!! no key candidate decrypts the pak index");
        return 1;
    }

    static int MovImmLength(byte[] b, int i)  // length of `mov dword [mem], imm32` at i, else 0
    {
        if (b[i] != 0xC7) return 0;
        int m = b[i + 1], mod = m >> 6, rm = m & 7;
        if ((m >> 3 & 7) != 0 || mod == 3 || (mod == 0 && rm == 5)) return 0;
        var len = 2 + (rm == 4 ? 1 : 0) + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
        return len + 4;
    }
}
