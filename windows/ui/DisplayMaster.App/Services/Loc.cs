using Microsoft.Windows.ApplicationModel.Resources;

namespace DisplayMaster.Services;

/// <summary>
/// Texts from Strings\&lt;language&gt;\Resources.resw (the same resources XAML reads through
/// x:Uid). Windows picks the language from the user's display language; English otherwise.
/// </summary>
public static class Loc
{
    private static readonly ResourceLoader? Loader = TryCreate();

    private static ResourceLoader? TryCreate()
    {
        try
        {
            return new ResourceLoader();
        }
        catch
        {
            return null;  // no resources (e.g. unit tests): the keys show instead of crashing
        }
    }

    public static string S(string key)
    {
        try
        {
            var s = Loader?.GetString(key);
            return string.IsNullOrEmpty(s) ? key : s;
        }
        catch
        {
            return key;
        }
    }

    public static string F(string key, params object[] args) => string.Format(S(key), args);
}
