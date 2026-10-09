using System.Buffers.Binary;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

return await BuildValidator.RunAsync(args);

internal static class BuildValidator
{
    public static async Task<int> RunAsync(string[] args)
    {
        try
        {
            var options = ParseArguments(args);
            var manifest = JsonSerializer.Deserialize<Manifest>(
                await File.ReadAllTextAsync(Path.GetFullPath(options.ManifestPath)),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                ?? throw new InvalidDataException("The manifest is empty.");

            if (manifest.SchemaVersion != 1 || manifest.Builds.Count == 0)
                throw new InvalidDataException("Unsupported or empty build manifest.");

            var x64Root = Path.Combine(Path.GetFullPath(options.GameRoot), "x64");
            var executablePath = Path.Combine(x64Root, "Grim Dawn.exe");
            var executableHash = await HashAsync(executablePath);
            var build = manifest.Builds.SingleOrDefault(candidate =>
                candidate.ExecutableSha256.Equals(executableHash, StringComparison.OrdinalIgnoreCase));
            if (build is null)
                throw new InvalidDataException($"Unsupported Grim Dawn executable hash: {executableHash}");

            foreach (var module in build.Modules)
                await ValidateModuleAsync(Path.Combine(x64Root, module.File), module);

            Console.WriteLine($"Supported build validated: {build.Id}");
            Console.WriteLine($"Validated {build.Modules.Sum(module => module.Exports.Count)} exported access points without opening the game process.");
            return 0;
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or InvalidDataException or UnauthorizedAccessException or JsonException)
        {
            Console.Error.WriteLine($"Build validation failed: {ex.Message}");
            return 1;
        }
    }

    private static async Task ValidateModuleAsync(string path, ModuleSpec spec)
    {
        var image = await File.ReadAllBytesAsync(path);
        var hash = Convert.ToHexStringLower(SHA256.HashData(image));
        if (!hash.Equals(spec.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{spec.File} hash is unsupported: {hash}");

        using var reader = new PEReader(new MemoryStream(image, writable: false));
        var headers = reader.PEHeaders;
        if (headers.PEHeader is null || headers.CoffHeader.Machine != Machine.Amd64 || headers.PEHeader.Magic != PEMagic.PE32Plus)
            throw new InvalidDataException($"{spec.File} is not AMD64 PE32+.");

        var exports = ReadExports(image, headers);
        foreach (var expected in spec.Exports)
        {
            if (!exports.TryGetValue(expected.Name, out var actualRva))
                throw new InvalidDataException($"{spec.File} is missing export {expected.Name}.");
            if (actualRva != expected.Rva)
                throw new InvalidDataException($"{spec.File}!{expected.Name} has unexpected RVA 0x{actualRva:x}.");

            byte[] prefix;
            try { prefix = Convert.FromHexString(expected.PrefixHex); }
            catch (FormatException ex) { throw new InvalidDataException($"Invalid prefix in manifest for {expected.Name}.", ex); }
            var offset = RvaToOffset(checked((int)actualRva), headers);
            if (offset > image.Length - prefix.Length || !image.AsSpan(offset, prefix.Length).SequenceEqual(prefix))
                throw new InvalidDataException($"{spec.File}!{expected.Name} failed code-prefix validation.");
        }
        foreach (var expected in spec.DataExports ?? [])
        {
            if (!exports.TryGetValue(expected.Name, out var actualRva) || actualRva != expected.Rva)
                throw new InvalidDataException($"{spec.File}!{expected.Name} failed data-export validation.");
        }
        foreach (var expected in spec.InternalCode ?? [])
        {
            byte[] prefix;
            try { prefix = Convert.FromHexString(expected.PrefixHex); }
            catch (FormatException ex) { throw new InvalidDataException($"Invalid internal prefix for {expected.Name}.", ex); }
            var offset = RvaToOffset(checked((int)expected.Rva), headers);
            if (prefix.Length == 0 || offset > image.Length - prefix.Length || !image.AsSpan(offset, prefix.Length).SequenceEqual(prefix))
                throw new InvalidDataException($"{spec.File}!{expected.Name} failed internal-code validation.");
        }
    }

    private static Dictionary<string, uint> ReadExports(byte[] image, PEHeaders headers)
    {
        var directory = headers.PEHeader!.ExportTableDirectory;
        if (directory.RelativeVirtualAddress == 0 || directory.Size < 40)
            throw new InvalidDataException("Module has no valid export directory.");
        var directoryOffset = RvaToOffset(directory.RelativeVirtualAddress, headers);
        var functionCount = ReadUInt32(image, directoryOffset + 20);
        var nameCount = ReadUInt32(image, directoryOffset + 24);
        if (nameCount > functionCount || nameCount > 1_000_000)
            throw new InvalidDataException("Unreasonable export-table counts.");
        var functionsOffset = RvaToOffset(checked((int)ReadUInt32(image, directoryOffset + 28)), headers);
        var namesOffset = RvaToOffset(checked((int)ReadUInt32(image, directoryOffset + 32)), headers);
        var ordinalsOffset = RvaToOffset(checked((int)ReadUInt32(image, directoryOffset + 36)), headers);
        var result = new Dictionary<string, uint>(StringComparer.Ordinal);
        for (var index = 0; index < nameCount; index++)
        {
            var nameRva = ReadUInt32(image, checked(namesOffset + (int)index * 4));
            var ordinalIndex = ReadUInt16(image, checked(ordinalsOffset + (int)index * 2));
            if (ordinalIndex >= functionCount) throw new InvalidDataException("Invalid export ordinal.");
            result.Add(ReadAsciiZ(image, RvaToOffset(checked((int)nameRva), headers)),
                ReadUInt32(image, checked(functionsOffset + ordinalIndex * 4)));
        }
        return result;
    }

    private static int RvaToOffset(int rva, PEHeaders headers)
    {
        foreach (var section in headers.SectionHeaders)
        {
            var size = Math.Max(section.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva < section.VirtualAddress + size)
                return checked(rva - section.VirtualAddress + section.PointerToRawData);
        }
        throw new InvalidDataException($"RVA 0x{rva:x} is outside all PE sections.");
    }

    private static ushort ReadUInt16(byte[] image, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(offset, 2));
    private static uint ReadUInt32(byte[] image, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(offset, 4));
    private static string ReadAsciiZ(byte[] image, int offset)
    {
        var end = Array.IndexOf(image, (byte)0, offset);
        if (end < 0) throw new InvalidDataException("Unterminated export name.");
        return Encoding.ASCII.GetString(image, offset, end - offset);
    }

    private static async Task<string> HashAsync(string path)
    {
        await using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(await SHA256.HashDataAsync(stream));
    }

    private static Options ParseArguments(string[] args)
    {
        string? gameRoot = null;
        string? manifest = null;
        for (var index = 0; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--game-root": gameRoot = RequireValue(args, ref index); break;
                case "--manifest": manifest = RequireValue(args, ref index); break;
                default: throw new ArgumentException($"Unknown argument: {args[index]}");
            }
        }
        return new Options(gameRoot ?? throw new ArgumentException("Supply --game-root."),
            manifest ?? throw new ArgumentException("Supply --manifest."));
    }

    private static string RequireValue(string[] args, ref int index)
    {
        if (++index >= args.Length) throw new ArgumentException($"Missing value after {args[index - 1]}.");
        return args[index];
    }

    private sealed record Options(string GameRoot, string ManifestPath);
    private sealed record Manifest(int SchemaVersion, IReadOnlyList<BuildSpec> Builds);
    private sealed record BuildSpec(string Id, string ExecutableSha256, IReadOnlyList<ModuleSpec> Modules);
    private sealed record ModuleSpec(string File, string Sha256, IReadOnlyList<ExportSpec> Exports,
        IReadOnlyList<ExportSpec>? InternalCode = null, IReadOnlyList<DataExportSpec>? DataExports = null);
    private sealed record DataExportSpec(string Name, uint Rva);
    private sealed record ExportSpec(string Name, uint Rva, string PrefixHex);
}
