using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;

namespace NGine;

// Funktionen der Engine für Skript-Komponenten; Aufbau wie ScriptApi in
// engine/script/script_api.cppm.
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public unsafe struct ScriptApi
{
    public delegate* unmanaged<byte*, int, ScriptFieldDesc*, int, int, int> RegisterScript;
    public delegate* unmanaged<int, int, long> FieldOffset;
    public delegate* unmanaged<int, void*> Prototype;
    public delegate* unmanaged<void*, byte**, int> StringGet;
    public delegate* unmanaged<void*, byte*, int, void> StringSet;
}

[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public unsafe struct ScriptFieldDesc
{
    public byte* Name;
    public int NameLength;
    public byte* Type;
    public int TypeLength;
    // Wie ngine::reflect::Hint: 0 keiner, 1 Farbe, 2 Drehung, 3 Winkel, 4 Asset.
    public int Hint;
    public int HasRange;
    public double Min;
    public double Max;
    public double Step;
    // 1: [Local] (R5.5).
    public int Local;
    public int Reserved;
}

// Ein Feld eines Skripts und wie es in den Speicher der Komponente kommt.
internal sealed unsafe class ScriptField
{
    public required FieldInfo Info;
    public required string TypeName;
    public long Offset = -1;
    // Zahlen, Enums und Structs: byteweise; Texte über die Engine.
    public Action<object, IntPtr>? Store;
    public Action<object, IntPtr>? Load;

    public static ScriptField? Create(FieldInfo info)
    {
        Type type = info.FieldType;
        string? name = RegistryName(type);
        if (name == null)
        {
            Log.Warn($"Skript {info.DeclaringType?.Name}: Feld {info.Name} hat einen Typ, den die Engine nicht kennt ({type.Name}), und wird nicht gespeichert");
            return null;
        }
        var field = new ScriptField { Info = info, TypeName = name };
        if (type == typeof(string))
        {
            field.Store = (o, p) => SetString((void*)p, (string?)info.GetValue(o) ?? "");
            field.Load = (o, p) => info.SetValue(o, GetString((void*)p));
        }
        else
        {
            MethodInfo store = typeof(ScriptField).GetMethod(nameof(StoreValue), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(type);
            MethodInfo load = typeof(ScriptField).GetMethod(nameof(LoadValue), BindingFlags.NonPublic | BindingFlags.Static)!.MakeGenericMethod(type);
            field.Store = (o, p) => store.Invoke(null, [info, o, p]);
            field.Load = (o, p) => load.Invoke(null, [info, o, p]);
        }
        return field;
    }

    private static void StoreValue<T>(FieldInfo info, object o, IntPtr p) where T : unmanaged
    {
        *(T*)p = (T)info.GetValue(o)!;
    }

    private static void LoadValue<T>(FieldInfo info, object o, IntPtr p) where T : unmanaged
    {
        info.SetValue(o, *(T*)p);
    }

    // Name des Feldtyps in der Reflection der Engine; null, wenn es ihn nicht gibt.
    private static string? RegistryName(Type type)
    {
        if (type == typeof(bool)) return "bool";
        if (type == typeof(sbyte)) return "i8";
        if (type == typeof(short)) return "i16";
        if (type == typeof(int)) return "i32";
        if (type == typeof(long)) return "i64";
        if (type == typeof(byte)) return "u8";
        if (type == typeof(ushort)) return "u16";
        if (type == typeof(uint)) return "u32";
        if (type == typeof(ulong)) return "u64";
        if (type == typeof(float)) return "f32";
        if (type == typeof(double)) return "f64";
        if (type == typeof(string)) return "string";
        // Enums und Structs nur aus NGine.Scripting (aus der Reflection erzeugt oder Mathe-Typen).
        if (type.Assembly != typeof(Script).Assembly)
        {
            return null;
        }
        if (type.IsEnum)
        {
            return type.Name;
        }
        if (type.IsValueType && typeof(IComponent).IsAssignableFrom(type))
        {
            return (string?)type.GetProperty("TypeName", BindingFlags.Public | BindingFlags.Static)?.GetValue(null);
        }
        return null;
    }

    internal static string GetString(void* str)
    {
        byte* text = null;
        int length = Host.ScriptApi->StringGet(str, &text);
        return length > 0 ? Encoding.UTF8.GetString(text, length) : "";
    }

    internal static void SetString(void* str, string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        fixed (byte* text = bytes)
        {
            Host.ScriptApi->StringSet(str, text, bytes.Length);
        }
    }
}

// Eine Skript-Klasse mit ihrem Komponententyp und ihren Instanzen.
internal sealed unsafe class ScriptClass
{
    public required Type Type;
    public required string Name;
    public required ScriptField[] Fields;
    // Wo die Klasse läuft (R5.5): [Client], [Everywhere] oder ohne Attribut nur auf dem Server.
    public NetPlacement Placement;
    // [Replicated]: Die Komponente geht mit den Schnappschüssen an die Clients.
    public bool Replicated;

    // Typ-Index und Instanzen pro Skript-Welt (R5.5); es gilt die gewählte (Host.Domain).
    private sealed class Slot
    {
        public int TypeIndex = -1;
        public readonly Dictionary<ulong, Script> Instances = new();
    }
    private readonly Dictionary<int, Slot> slots = new();
    private Slot? current;
    private int currentDomain = -1;

    private Slot Current
    {
        get
        {
            if (current == null || currentDomain != Host.Domain)
            {
                if (!slots.TryGetValue(Host.Domain, out current))
                {
                    current = new Slot();
                    slots[Host.Domain] = current;
                }
                currentDomain = Host.Domain;
            }
            return current;
        }
    }

    public int TypeIndex
    {
        get => Current.TypeIndex;
        set => Current.TypeIndex = value;
    }
    public Dictionary<ulong, Script> Instances => Current.Instances;

    // Vergisst eine Skript-Welt; ihre Instanzen sind schon gestoppt.
    public void Forget(int domain)
    {
        slots.Remove(domain);
        current = null;
    }

    // Ob die Klasse in einer Skript-Welt mit dieser Rolle läuft.
    public bool RunsAs(NetRole role) => role switch
    {
        NetRole.Server => Placement != NetPlacement.Client,
        NetRole.Client => Placement != NetPlacement.Server,
        _ => true,
    };

    public static ScriptClass Create(Type type)
    {
        var fields = new List<ScriptField>();
        foreach (FieldInfo info in type.GetFields(BindingFlags.Public | BindingFlags.Instance))
        {
            if (info.IsInitOnly || info.IsLiteral)
            {
                continue;
            }
            if (ScriptField.Create(info) is { } field)
            {
                fields.Add(field);
            }
        }
        NetPlacement placement = type.GetCustomAttribute<ClientAttribute>() != null ? NetPlacement.Client
            : type.GetCustomAttribute<EverywhereAttribute>() != null ? NetPlacement.Everywhere
            : NetPlacement.Server;
        return new ScriptClass
        {
            Type = type,
            Name = type.Name,
            Fields = fields.ToArray(),
            Placement = placement,
            Replicated = type.GetCustomAttribute<ReplicatedAttribute>() != null,
        };
    }

    // Meldet den Typ bei der Engine und schreibt die Standardwerte (Feldinitialisierer) in die
    // Vorlage. false, wenn die Engine ihn ablehnt.
    public bool Register()
    {
        var descs = new ScriptFieldDesc[Fields.Length];
        // Die Byte-Arrays bleiben während des Aufrufs fest (GCHandle), damit die Zeiger gelten.
        var handles = new List<System.Runtime.InteropServices.GCHandle>();
        try
        {
            for (int i = 0; i < Fields.Length; ++i)
            {
                ScriptField field = Fields[i];
                byte[] name = Encoding.UTF8.GetBytes(field.Info.Name);
                byte[] typeName = Encoding.UTF8.GetBytes(field.TypeName);
                var nameHandle = System.Runtime.InteropServices.GCHandle.Alloc(name, System.Runtime.InteropServices.GCHandleType.Pinned);
                var typeHandle = System.Runtime.InteropServices.GCHandle.Alloc(typeName, System.Runtime.InteropServices.GCHandleType.Pinned);
                handles.Add(nameHandle);
                handles.Add(typeHandle);
                descs[i].Name = (byte*)nameHandle.AddrOfPinnedObject();
                descs[i].NameLength = name.Length;
                descs[i].Type = (byte*)typeHandle.AddrOfPinnedObject();
                descs[i].TypeLength = typeName.Length;
                if (field.Info.GetCustomAttribute<RangeAttribute>() is { } range)
                {
                    descs[i].HasRange = 1;
                    descs[i].Min = range.Min;
                    descs[i].Max = range.Max;
                    descs[i].Step = range.Step;
                }
                if (field.Info.GetCustomAttribute<ColorAttribute>() != null) descs[i].Hint = 1;
                if (field.Info.GetCustomAttribute<AssetAttribute>() != null) descs[i].Hint = 4;
                if (field.Info.GetCustomAttribute<LocalAttribute>() != null) descs[i].Local = 1;
            }
            byte[] className = Encoding.UTF8.GetBytes(Name);
            fixed (byte* classText = className)
            fixed (ScriptFieldDesc* fieldData = descs)
            {
                TypeIndex = Host.ScriptApi->RegisterScript(classText, className.Length, fieldData, descs.Length,
                                                          Replicated ? 1 : 0);
            }
        }
        finally
        {
            foreach (var handle in handles)
            {
                handle.Free();
            }
        }
        if (TypeIndex < 0)
        {
            return false;
        }
        for (int i = 0; i < Fields.Length; ++i)
        {
            Fields[i].Offset = Host.ScriptApi->FieldOffset(TypeIndex, i);
        }
        // Standardwerte aus einer frischen Instanz.
        object fresh = Activator.CreateInstance(Type)!;
        Store(fresh, Host.ScriptApi->Prototype(TypeIndex));
        return true;
    }

    public void Store(object script, void* data)
    {
        foreach (ScriptField field in Fields)
        {
            field.Store!(script, (IntPtr)((byte*)data + field.Offset));
        }
    }

    public void Load(object script, void* data)
    {
        foreach (ScriptField field in Fields)
        {
            field.Load!(script, (IntPtr)((byte*)data + field.Offset));
        }
    }
}
