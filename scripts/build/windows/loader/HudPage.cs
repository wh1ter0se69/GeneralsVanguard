/* The HUD page: readout sizes, the income column, and one keyboard binding.
 *
 * GeneralsX @feature Claude 13/08/2026
 *
 * WHY EVERY SIZE ON THIS PAGE NOW GETS A PREVIEW. "How big is 14?" has no answer a person can hold
 * in their head. The message list was the first to get a picture because it is the text a player
 * reads mid-fight - "Unit under attack" arriving while you are looking somewhere else - and picking
 * that number blind means launching the game, starting a skirmish, waiting for a message, quitting,
 * and guessing again.
 *
 * THIS HEADER USED TO ARGUE THE OTHER FIVE SHOULD NOT HAVE ONE: "corner numbers you glance at; a
 * preview of each would be five boxes of noise around the one that matters." That was a claim about
 * ATTENTION and it is still true about attention - which is why HudReadoutPreview is ONE picture cut
 * into five labelled rows rather than five separate boxes. It was wrong about information. A spin box
 * reading 8 says nothing about what 8 looks like, and 8 is not even the size the game draws: at 2560
 * wide the engine multiplies it by 2.54. Five numbers with no picture was not a calm page, it was
 * five settings nobody could choose deliberately.
 *
 * AND THE MESSAGE PREVIEW SAT ON FLAT BLACK, which was the worse half of the same fault. The game
 * draws those lines over terrain, units and explosions with nothing behind them but a one-pixel
 * black drop shadow, and white text on a black rectangle is legible at every size a person could
 * type - so the preview answered "yes, readable" to every number it was ever shown. It now sits on a
 * real frame of the game (see HudGround.cs), with the frame-rate readout above it because that is
 * its actual neighbour in that corner, and the message list alternates white and grey the way the
 * engine really does, because the grey line is the one that fails first.
 *
 * THE PREVIEW HAD TO DO ARITHMETIC OR IT WOULD HAVE LIED BY ABOUT A FACTOR OF TWO. The engine does
 * not draw the number you type: it draws
 * TheGlobalLanguageData->adjustFontSize(m_messagePointSize) (InGameUI.cpp:2587), and at 1920 wide
 * with the shipped scaler that multiplies by 1.98. A preview showing "14" at 14 points would have
 * been confidently, consistently wrong - which is worse than no preview, because the user would
 * have trusted it. See HudPrefs.ResolutionScale.
 *
 * WHAT IT STILL CANNOT KNOW, and says so on screen rather than quietly assuming: the message FACE
 * and size can be overridden by the language pack, which lives inside the game's .big archives -
 * files this launcher deliberately never opens. So the preview states what it assumed instead of
 * presenting a guess as a fact.
 */

using System.Diagnostics;
using System.Globalization;

namespace GeneralsX.Loader;

internal sealed partial class OptionsPanel
{
    // ---------------------------------------------------------------------------------------
    // Pending values. Same discipline as _resolution and _debugMenu: null means "the user never
    // touched this control", and nothing at all is written for it. See the SAVE handler in
    // OptionsForm.cs for why a value must also be CLEARED once written.
    // ---------------------------------------------------------------------------------------

    private int? _msgSize;
    private int? _latencySize;
    private int? _fpsSize;
    private int? _playerListSize;
    private int? _sysTimeSize;
    private int? _gameTimeSize;
    private bool? _moneyPerMinute;

    /// <summary>
    /// The pending HUD anchor. Same null-means-untouched rule as every field above it, but these two
    /// go to a DIFFERENT FILE with a different owner - GXHud.ini, which the engine only ever reads
    /// (gxHudSetting opens it "r", InGameUI.cpp:1015) and never writes back. So unlike the sizes,
    /// nothing about a running game can undo them; see SaveHud.
    /// </summary>
    private int? _anchorX;
    private int? _anchorY;

    private HudAnchorPreview? _anchorPreview;

    /// <summary>
    /// The pending keyboard binding. This one needs two fields rather than a nullable pair, because
    /// "remove the binding" is a real pending action and is not the same as "unchanged" - a single
    /// null could not tell them apart.
    /// </summary>
    private bool _bindPending;

    // The whole binding set as the page would write it, keyed by command name. A command ABSENT
    // from this map is unbound - that is the same representation Read returns and Write consumes,
    // so "remove one of the two" needs no separate signal.
    private Dictionary<string, (string Key, string Modifiers)>? _bind;

    private MessagePreview? _preview;
    private Label? _previewCaption;
    private Label? _runningWarn;
    private HudReadoutPreview? _readoutPreview;

    /// <summary>True while a game is running, so writing the game's Options.ini would be undone.</summary>
    private static bool GameIsRunning()
    {
        try
        {
            // Match by process NAME. The exe is generalszh.exe (Apply.GameExe) and it is started
            // through GeneralsX.cmd, so a command-line match would be looking at the wrong string.
            //
            // KNOWN GAP, stated rather than papered over: under an engine packaged before
            // 23/09/2026, retail Zero Hour and the Generals Online client share this Options.ini and
            // run under different names, so a session of either one is NOT detected here and can
            // still overwrite a save on exit. Guessing at their executable names would be a list that
            // looks complete and is not. A newer engine keeps its own folder (OwnedPaths.UserDataDir, from its first start on),
            // and neither of them writes there.
            var found = Process.GetProcessesByName("generalszh");
            foreach (var p in found)
            {
                p.Dispose();
            }
            return found.Length > 0;
        }
        catch
        {
            // Refused by policy on a locked-down box. Not knowing must not block a save; the
            // engine's own exit-write is still the backstop, and the user is told what to expect.
            return false;
        }
    }

    /// <summary>
    /// THE HAZARD, stated where it is handled rather than only in a comment.
    ///
    /// The game loads Options.ini into a std::map once at startup and dumps that whole map back
    /// over the file when it exits (UserPreferences::load / ::write). It does not merge. So a
    /// launcher write that happens while the game is running is REVERTED the moment the game
    /// quits - keys we added vanish, keys we changed go back to the values the game read at
    /// startup - and the user sees a setting they saved that did not take, with nothing anywhere
    /// saying why.
    ///
    /// Refusing the write is therefore not caution, it is the only outcome that is not a lie. The
    /// pending values are LEFT pending, so closing the game and pressing SAVE again applies them.
    /// </summary>
    private bool BlockedByRunningGame()
    {
        if (!GameIsRunning())
        {
            return false;
        }
        MessageBox.Show(this,
            "Zero Hour is running, so these settings were NOT saved.\n\n" +
            "The game keeps its own copy of Options.ini in memory and writes the whole file back " +
            "when it exits, which would undo anything saved now.\n\n" +
            "Close the game and press SAVE again - your choices are still here.",
            "GeneralsX", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        return true;
    }

    /// <summary>Everything the HUD page wants written to the game's Options.ini, in file order.</summary>
    private List<KeyValuePair<string, string>> PendingHudKeys()
    {
        var pairs = new List<KeyValuePair<string, string>>();
        void Add(string key, int? v)
        {
            if (v is { } n)
            {
                pairs.Add(new KeyValuePair<string, string>(key, n.ToString(CultureInfo.InvariantCulture)));
            }
        }
        Add(HudPrefs.InGameMessageKey, _msgSize);
        Add(HudPrefs.NetworkLatencyKey, _latencySize);
        Add(HudPrefs.RenderFpsKey, _fpsSize);
        Add(HudPrefs.PlayerInfoListKey, _playerListSize);
        Add(HudPrefs.SystemTimeKey, _sysTimeSize);
        Add(HudPrefs.GameTimeKey, _gameTimeSize);
        if (_moneyPerMinute is { } m)
        {
            // "yes"/"no" and nothing else. OptionPreferences::getShowMoneyPerMinute compares with
            // stricmp against "yes" only - it is NOT UserPreferences::getBool - so "1" and "true"
            // would both read back as OFF and the checkbox would not survive a round trip.
            pairs.Add(new KeyValuePair<string, string>(HudPrefs.MoneyPerMinuteKey, m ? "yes" : "no"));
        }
        return pairs;
    }

    /// <summary>
    /// Apply the HUD page's pending values. Called from SAVE, after the running-game gate.
    /// Each group clears only on success, so a refused write stays pending and is retried.
    /// </summary>
    private void SaveHud()
    {
        var pairs = PendingHudKeys();
        if (pairs.Count > 0 && HudPrefs.WriteKeys(OwnedPaths.OptionsIni, pairs))
        {
            _msgSize = null;
            _latencySize = null;
            _fpsSize = null;
            _playerListSize = null;
            _sysTimeSize = null;
            _gameTimeSize = null;
            _moneyPerMinute = null;
        }

        // The HUD anchor, in GXHud.ini. Third file, third owner, and the same reasoning as the
        // keybinding below it: the engine opens it "r" and there is no writer anywhere in the tree
        // (InGameUI.cpp:1015 is the only reference to the name), so there is no in-memory copy
        // waiting to be dumped back over this when the game exits. Nothing here is gated on the game
        // being closed.
        //
        // EITHER PENDING VALUE WRITES BOTH KEYS, so the one that was not touched has to be filled in
        // from disk rather than from a default - otherwise moving the block sideways would silently
        // reset a vertical offset the user set earlier and had every reason to expect to keep.
        if (_anchorX is not null || _anchorY is not null)
        {
            var cur = GxHud.ReadAnchor(OwnedPaths.GxHudIni);
            if (GxHud.WriteAnchor(OwnedPaths.GxHudIni, _anchorX ?? cur.X, _anchorY ?? cur.Y))
            {
                _anchorX = null;
                _anchorY = null;
            }
        }

        // The keybinding is a DIFFERENT file with a different owner, and the game does not rewrite
        // it on exit - it only reads it at startup. So it is not gated on the game being closed,
        // and it is saved even when the Options.ini half was refused.
        if (_bindPending && _bind is { } b)
        {
            // Write deletes the file itself when the set is empty, so both keys being cleared puts
            // the install back to having no GXUserKeys.ini at all.
            if (GameCommandMap.Write(OwnedPaths.InstallDir, b))
            {
                _bindPending = false;
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // The page
    // ---------------------------------------------------------------------------------------

    private void BuildHud()
    {
        var ini = OwnedPaths.OptionsIni;

        // Select() cleared the page, so every control field below is stale until reassigned.
        _preview = null;
        _previewCaption = null;
        _runningWarn = null;
        _anchorPreview = null;
        _readoutPreview = null;

        if (GameIsRunning())
        {
            _runningWarn = new Label
            {
                AutoSize = false, Size = new Size(RowWidth, 40), UseMnemonic = false,
                ForeColor = Color.FromArgb(0xE8, 0xB3, 0x39), Font = new Font("Segoe UI", 9f),
                TextAlign = ContentAlignment.TopLeft,
                Text =
                    "Zero Hour is running. It rewrites Options.ini when it exits, so anything saved\n" +
                    "now would be undone. Close the game first - SAVE will tell you if you forget.",
            };
            Row(_runningWarn, 46);
        }

        // -----------------------------------------------------------------------------------
        Note("In-game messages   (\"Unit under attack\", \"Upgrade Complete\", defeat notices)");

        var msgNow = HudPrefs.ReadSize(ini, HudPrefs.InGameMessageKey, HudPrefs.MessageUseGameValue);
        var res = HudPrefs.ReadResolution(ini);
        var scaler = HudPrefs.ReadFontScaler(ini);

        // Read once, up here rather than beside the section that owns them, because BOTH previews
        // need the EFFECTIVE size of every readout - the pending edit when there is one, the saved
        // value otherwise - and the message preview above draws the frame-rate readout as its
        // neighbour. Going back to the file on every keystroke would be a file read per keypress.
        var latencyNow = HudPrefs.ReadSize(ini, HudPrefs.NetworkLatencyKey, HudPrefs.ReadoutDefault);
        var fpsNow = HudPrefs.ReadSize(ini, HudPrefs.RenderFpsKey, HudPrefs.ReadoutDefault);
        var playerListNow = HudPrefs.ReadSize(ini, HudPrefs.PlayerInfoListKey, HudPrefs.ReadoutDefault);
        var sysTimeNow = HudPrefs.ReadSize(ini, HudPrefs.SystemTimeKey, HudPrefs.ReadoutDefault);
        var gameTimeNow = HudPrefs.ReadSize(ini, HudPrefs.GameTimeKey, HudPrefs.ReadoutDefault);

        _preview = new MessagePreview
        {
            Size = new Size(RowWidth, 150),
            // The resolution PENDING on the Graphics page wins over the saved one, so changing the
            // resolution and then coming here previews the size at the resolution about to apply
            // rather than the one being left behind.
            Display = _resolution is { } r ? (r.Width, r.Height) : res,
            Scaler = scaler,
            Chosen = _msgSize ?? msgNow,
            FpsSize = _fpsSize ?? fpsNow,
        };
        // The fit is only discovered while painting, so the caption is corrected from there rather
        // than being written once and left to go stale.
        _preview.FitChanged += RefreshPreviewCaption;
        Row(_preview, 158);

        _previewCaption = new Label
        {
            AutoSize = false, Size = new Size(RowWidth, 34), UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9f),
            TextAlign = ContentAlignment.TopLeft,
        };
        Row(_previewCaption, 38);

        SizeRow("Message size", _msgSize ?? msgNow, 0, HudPrefs.MaxFontSize, v =>
        {
            _msgSize = v;
            if (_preview is not null)
            {
                _preview.Chosen = v;
            }
            RefreshPreviewCaption();
        });

        Note("0 means \"leave it as the game has it\", which is how every install starts.");
        RefreshPreviewCaption();

        // -----------------------------------------------------------------------------------
        Note("Other HUD readouts");

        // Deferred by design: neither preview exists yet when the rows below are built, and both are
        // rebuilt every time the page is selected. Reading the fields at CALL time rather than
        // capturing the controls means these handlers survive both.
        //
        // THE MESSAGE PREVIEW IS IN HERE TOO, which looks out of place until you remember it draws
        // the frame-rate readout as the message list's neighbour. Leaving it out made the fps spin
        // box change one picture on this page and not the other.
        void PushSizes()
        {
            var latency = _latencySize ?? latencyNow;
            var fps = _fpsSize ?? fpsNow;
            var playerList = _playerListSize ?? playerListNow;
            var sysTime = _sysTimeSize ?? sysTimeNow;
            var gameTime = _gameTimeSize ?? gameTimeNow;

            _anchorPreview?.SetSizes(latency, fps, sysTime, gameTime);
            _readoutPreview?.SetSizes(latency, fps, playerList, sysTime, gameTime);
            if (_preview is not null)
            {
                _preview.FpsSize = fps;
            }
        }

        var moneyNow = _moneyPerMinute ?? HudPrefs.ReadMoneyPerMinute(ini, false);

        // The picture goes ABOVE the numbers it explains, the same way it does for the message list.
        // Underneath, a player has to scroll past five spin boxes before finding out what they did.
        _readoutPreview = new HudReadoutPreview(RowWidth)
        {
            Scaler = scaler,
            MoneyPerMinute = moneyNow,
        };
        _readoutPreview.SetScreen(_resolution is { } r2 ? (r2.Width, r2.Height) : res);
        PushSizes();
        Row(_readoutPreview, _readoutPreview.Height + 8);

        SizeRow("Ping / latency", _latencySize ?? latencyNow,
            0, HudPrefs.MaxFontSize, v => { _latencySize = v; PushSizes(); });
        SizeRow("Frame rate", _fpsSize ?? fpsNow,
            0, HudPrefs.MaxFontSize, v => { _fpsSize = v; PushSizes(); });
        SizeRow("Player list", _playerListSize ?? playerListNow,
            0, HudPrefs.MaxFontSize, v => { _playerListSize = v; PushSizes(); });
        SizeRow("System clock", _sysTimeSize ?? sysTimeNow,
            0, HudPrefs.MaxFontSize, v => { _sysTimeSize = v; PushSizes(); });
        SizeRow("Match timer", _gameTimeSize ?? gameTimeNow,
            0, HudPrefs.MaxFontSize, v => { _gameTimeSize = v; PushSizes(); });

        Row(Check("Show income per minute in the player list",
            moneyNow,
            v =>
            {
                _moneyPerMinute = v;
                // This checkbox adds or removes the "+" column from the player list
                // (InGameUI.cpp:6952, :7017), and until the preview existed it changed nothing
                // anybody could see without starting a match.
                if (_readoutPreview is not null)
                {
                    _readoutPreview.MoneyPerMinute = v;
                }
            }));

        Note("These five default to 8. They are scaled by your resolution the same way the message");
        Note("text is, so the same number looks bigger on a bigger screen. 0 hides one completely.");
        Note("Ping and the resend counter next to it only appear in a multiplayer match, and on the");
        Note("packet router the ping also names the peer it is timing — \"47ms Ada\" rather than \"47ms\".");
        Note("The player list only appears while you are observing. The match timer is drawn at the");
        Note("RIGHT edge of the screen, which is why the preview puts it there.");

        // -----------------------------------------------------------------------------------
        BuildHudAnchor(res, scaler, PushSizes);

        // -----------------------------------------------------------------------------------
        Note("Hide the HUD with a key - two independent toggles");
        BuildHudKeyBinding();

        Note("Everything on this page applies the next time the game starts.");
    }

    /// <summary>
    /// The HUD anchor: two numbers, a picture of what they do, and a way back.
    ///
    /// WHY THIS SECTION EXISTS AT ALL. The engine gained a settable origin for the whole top-left
    /// readout stack (InGameUI.cpp:984-1058) and reads it from GXHud.ini exactly once per process,
    /// so without a launcher surface the only way to tune it is to hand-edit a file in Documents and
    /// restart the game to see the result. That is the loop the preview removes.
    ///
    /// ONE ANCHOR, NOT SIX POSITIONS, AND THAT IS THE ENGINE'S CHOICE RATHER THAN A SIMPLIFICATION
    /// MADE HERE. Each readout still has its own position member, but they all draw inline off the
    /// shared anchor while they SIT at it (isAtHudAnchorPos, :1082) - the engine's own comment at
    /// :988-990 records that scattering them individually turns a stack into a layout nobody can
    /// reset. Offering six pairs of spin boxes would be offering a state the engine deliberately
    /// declines to support.
    ///
    /// THE MATCH TIMER IS THE ONE SURPRISE AND IT IS SAID OUT LOUD BELOW. drawGameTime insets from
    /// the RIGHT edge (:6932-6936), so the same "across" number that pushes the ping counter right
    /// pushes the clock LEFT. A player who was not told would read that as a bug.
    /// </summary>
    private void BuildHudAnchor((int Width, int Height)? res, double scaler, Action pushSizes)
    {
        Note("Where the readouts sit   (ping, resends, frame rate, tps, system clock, match timer)");

        var saved = GxHud.ReadAnchor(OwnedPaths.GxHudIni);
        var x0 = _anchorX ?? saved.X;
        var y0 = _anchorY ?? saved.Y;

        _anchorPreview = new HudAnchorPreview(RowWidth)
        {
            Scaler = scaler,
            AnchorX = x0,
            AnchorY = y0,
        };
        // The resolution PENDING on the Graphics page wins over the saved one, exactly as it does for
        // the message preview above: previewing at the resolution about to apply is the useful
        // answer, and the two previews disagreeing about which screen they are describing would be
        // worse than either of them being slightly stale.
        _anchorPreview.SetScreen(_resolution is { } r ? (r.Width, r.Height) : res);
        pushSizes();
        Row(_anchorPreview, _anchorPreview.Height + 8);

        var spinX = SizeRow("Across, from the left edge", x0, GxHud.MinAnchor, GxHud.MaxAnchor, v =>
        {
            _anchorX = v;
            if (_anchorPreview is not null)
            {
                _anchorPreview.AnchorX = v;
            }
        });
        var spinY = SizeRow("Down, from the top edge", y0, GxHud.MinAnchor, GxHud.MaxAnchor, v =>
        {
            _anchorY = v;
            if (_anchorPreview is not null)
            {
                _anchorPreview.AnchorY = v;
            }
        });

        var reset = new BigButton("RESET POSITION", LauncherTheme.Bg, LauncherTheme.Border)
        {
            Ghost = true, Size = new Size(200, 36),
        };
        reset.Click += (_, _) =>
        {
            spinX.Value = GxHud.DefaultAnchorX;
            spinY.Value = GxHud.DefaultAnchorY;
            // Set the pending values and the preview EXPLICITLY rather than relying on ValueChanged.
            // Assigning a value a box already holds raises no event, so resetting an already-default
            // screen would mark nothing pending and SAVE would write nothing at all - the same trap
            // the camera page's RESTORE DEFAULTS records.
            _anchorX = GxHud.DefaultAnchorX;
            _anchorY = GxHud.DefaultAnchorY;
            if (_anchorPreview is not null)
            {
                _anchorPreview.AnchorX = GxHud.DefaultAnchorX;
                _anchorPreview.AnchorY = GxHud.DefaultAnchorY;
            }
        };
        Row(reset, 44, wide: false);

        Note($"Defaults are {GxHud.DefaultAnchorX} across and {GxHud.DefaultAnchorY} down. The negative lifts the row one pixel above");
        Note("the top edge, which is where these readouts have always sat — it is not a mistake.");
        Note("The match timer is anchored to the RIGHT edge, so \"across\" moves it in from that side");
        Note("rather than to the right. The preview shows both ends.");
        Note("Saved to GXHud.ini beside your Options.ini. Nothing else reads that file, it cannot affect");
        Note("who you can play with, and deleting it puts the defaults back.");
    }

    /// <summary>
    /// A label and a spin box on ONE row. Five settings stacked as label-above-box would be 300
    /// pixels of a 390 pixel viewport and would push the preview off the top of the page.
    /// </summary>
    private NumericUpDown SizeRow(string label, int value, int min, int max, Action<int> set)
    {
        var host = new Panel { Size = new Size(RowWidth, 30), BackColor = LauncherTheme.Bg };
        host.Controls.Add(new Label
        {
            Text = label, AutoSize = false, Bounds = new Rectangle(0, 4, 250, 22), UseMnemonic = false,
            ForeColor = LauncherTheme.Text, Font = new Font("Segoe UI", 9.5f),
            TextAlign = ContentAlignment.MiddleLeft,
        });
        var spin = new NumericUpDown
        {
            Minimum = min, Maximum = max, Value = Math.Clamp(value, min, max),
            Bounds = new Rectangle(258, 2, 82, 26),
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle,
        };
        spin.ValueChanged += (_, _) => set((int)spin.Value);
        host.Controls.Add(spin);
        Row(host, 32);
        return spin;
    }

    /// <summary>
    /// The one line that says what the game will actually draw, and everything the launcher had to
    /// assume to say it. The assumptions are on screen because the alternative is a number that
    /// looks authoritative and is not.
    /// </summary>
    private void RefreshPreviewCaption()
    {
        if (_previewCaption is null || _preview is null)
        {
            return;
        }

        var chosen = _preview.Chosen;
        var display = _preview.Display;

        if (display is not { } d)
        {
            _previewCaption.Text =
                "Shown at the size you picked. The game scales HUD text by your resolution, and\n" +
                "Options.ini has no Resolution yet - set one on the Graphics page for a true preview.";
            return;
        }

        var effective = chosen > 0 ? chosen : HudPrefs.EngineBuiltInMessageSize;
        var drawn = HudPrefs.AdjustFontSize(effective, d.Width, d.Height, _preview.Scaler);
        var factor = HudPrefs.ResolutionScale(d.Width, d.Height, _preview.Scaler);

        var first = chosen > 0
            ? $"{chosen} pt in Options.ini  ->  drawn at {drawn} pt  ({factor.ToString("0.00", CultureInfo.InvariantCulture)}x for {d.Width} x {d.Height})"
            : $"Using the game's own size. Shown at the engine's built-in {HudPrefs.EngineBuiltInMessageSize} pt -> {drawn} pt; your language pack may set a different one.";

        var second = _preview.FitPercent < 100
            ? $"Too big for this box - shown at {_preview.FitPercent}% of actual size. Face assumed to be {HudPrefs.EngineBuiltInMessageFont}."
            : $"Actual size, in game pixels. Face assumed to be {HudPrefs.EngineBuiltInMessageFont}; a language pack can change it.";

        _previewCaption.Text = first + "\n" + second;
    }

    /// <summary>
    /// The two hide-HUD bindings, one row each.
    ///
    /// UNBOUND BY DEFAULT, AND THAT IS THE SAFE DEFAULT RATHER THAN A MISSING FEATURE. The engine
    /// checks bindings by walking a linked list and stopping at the FIRST match
    /// (MetaEvent.cpp:574-600), and records are pushed onto the FRONT as they are created
    /// (:720-721). This file is parsed after the game's own CommandMap.ini, so the record it
    /// creates sits at the head of that list and BEATS an existing binding on the same
    /// key-plus-modifier. Shipping a default would therefore silently take a key away from
    /// somebody, and the launcher cannot read the game's CommandMap.ini to check - it is inside
    /// INIZH.big.
    ///
    /// THE ROWS SHARE ONE PENDING SET. Both write into the same file, so editing one row and
    /// saving must not drop the other row's binding; _bind is seeded from what is already on disk
    /// and each row edits its own entry in it.
    /// </summary>
    private void BuildHudKeyBinding()
    {
        // Seeded from disk once, then owned by the page. Every row reads and writes this same map,
        // which is what stops row two's save from erasing row one.
        var pending = _bindPending && _bind is not null
            ? _bind
            : GameCommandMap.Read(OwnedPaths.InstallDir);
        var onDisk = new Dictionary<string, (string Key, string Modifiers)>(pending, StringComparer.Ordinal);

        foreach (var binding in GameCommandMap.All)
        {
            BuildOneKeyBinding(binding, pending, onDisk);
        }

        Note("If the game already uses a key you pick, THIS wins and the original stops working.");
        Note("Written to Data\\INI\\CommandMap\\GXUserKeys.ini in the install folder. Nothing else in");
        Note("your key setup is touched, updates leave it alone, and it cannot affect who you can play with.");
    }

    /// <summary>One command's label, key box and modifier box.</summary>
    private void BuildOneKeyBinding(
        GameCommandMap.Binding binding,
        Dictionary<string, (string Key, string Modifiers)> pending,
        IReadOnlyDictionary<string, (string Key, string Modifiers)> onDisk)
    {
        var wasBound = onDisk.ContainsKey(binding.Command);
        var current = pending.TryGetValue(binding.Command, out var cur) ? cur : ((string, string)?)null;

        // LauncherTheme.Text rather than Note()'s TextDim: this names the control below it, and the
        // dim colour is what the page uses for explanatory prose.
        Row(new Label
        {
            Text = binding.Title, AutoSize = false, Size = new Size(RowWidth, 22), UseMnemonic = false,
            ForeColor = LauncherTheme.Text, Font = new Font("Segoe UI", 9.5f),
            TextAlign = ContentAlignment.MiddleLeft,
        }, 24);

        var host = new Panel { Size = new Size(RowWidth, 32), BackColor = LauncherTheme.Bg };

        var keyBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
            Bounds = new Rectangle(0, 2, 150, 26), Font = new Font("Segoe UI", 9.5f),
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
        };
        keyBox.Items.Add("(not bound)");
        foreach (var k in GameCommandMap.Keys)
        {
            keyBox.Items.Add(Pretty(k));
        }

        var modBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
            Bounds = new Rectangle(158, 2, 150, 26), Font = new Font("Segoe UI", 9.5f),
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
        };
        foreach (var m in GameCommandMap.Modifiers)
        {
            modBox.Items.Add(m == "NONE" ? "(no modifier)" : m.Replace('_', '+'));
        }

        var keyIndex = 0;
        var modIndex = 0;
        if (current is { } c)
        {
            var ki = Array.IndexOf(GameCommandMap.Keys, c.Item1);
            keyIndex = ki >= 0 ? ki + 1 : 0;
            var mi = Array.IndexOf(GameCommandMap.Modifiers, c.Item2);
            modIndex = mi >= 0 ? mi : 0;
        }
        else
        {
            // Nothing bound: pre-select the suggestion's modifier so that choosing a key is one
            // click, while leaving the key box on "(not bound)" so opening this page changes nothing.
            var mi = Array.IndexOf(GameCommandMap.Modifiers, binding.SuggestedModifiers);
            modIndex = mi >= 0 ? mi : 0;
        }
        keyBox.SelectedIndex = keyIndex;
        modBox.SelectedIndex = modIndex;

        void Changed()
        {
            // Touching the modifier box while this command is unbound and was unbound on disk is
            // not a change - without this it would arm a pending write for a binding nobody asked
            // for, and SAVE would report work it did not do.
            if (keyBox.SelectedIndex <= 0 && !wasBound && !pending.ContainsKey(binding.Command))
            {
                return;
            }
            _bindPending = true;
            if (keyBox.SelectedIndex <= 0)
            {
                pending.Remove(binding.Command);     // pending REMOVAL of this one, not of the file
            }
            else
            {
                pending[binding.Command] =
                    (GameCommandMap.Keys[keyBox.SelectedIndex - 1],
                     GameCommandMap.Modifiers[Math.Max(0, modBox.SelectedIndex)]);
            }
            _bind = pending;
        }

        // Wired after the initial selection, so filling the boxes is not mistaken for a choice.
        keyBox.SelectedIndexChanged += (_, _) => Changed();
        modBox.SelectedIndexChanged += (_, _) => Changed();

        host.Controls.Add(keyBox);
        host.Controls.Add(modBox);
        Row(host, 36);

        foreach (var n in binding.Notes)
        {
            Note(n);
        }
        Note($"Suggested: {Pretty(binding.SuggestedKey)}" +
             (binding.SuggestedModifiers == "NONE" ? "." : $" with {binding.SuggestedModifiers.Replace('_', '+')}."));
    }

    /// <summary>"KEY_KPPLUS" reads as "Numpad +" to a person and as nothing at all as-is.</summary>
    private static string Pretty(string keyName)
    {
        var s = keyName.StartsWith("KEY_", StringComparison.Ordinal) ? keyName[4..] : keyName;
        if (s.StartsWith("KP", StringComparison.Ordinal))
        {
            var tail = s[2..];
            return "Numpad " + tail switch
            {
                "PLUS" => "+",
                "MINUS" => "-",
                "STAR" => "*",
                "SLASH" => "/",
                "DEL" => "Del",
                "ENTER" => "Enter",
                _ => tail,
            };
        }
        return s switch
        {
            "LBRACKET" => "[",
            "RBRACKET" => "]",
            "SEMICOLON" => ";",
            "APOSTROPHE" => "'",
            "TICK" => "`",
            "BACKSLASH" => "\\",
            "COMMA" => ",",
            "PERIOD" => ".",
            "SLASH" => "/",
            "MINUS" => "-",
            "EQUAL" => "=",
            "PGUP" => "Page Up",
            "PGDN" => "Page Down",
            "INS" => "Insert",
            "DEL" => "Delete",
            _ => s.Length == 1 ? s : s[0] + s[1..].ToLowerInvariant(),
        };
    }
}

/// <summary>
/// Three real messages at the size the game will really draw them.
///
/// THE SAMPLES ARE THE ONES A PLAYER ACTUALLY SEES, not lorem ipsum: a short alert, a long upgrade
/// line, and a defeat notice carrying a player name. The three differ in length on purpose, because
/// the question a size answers is partly "does the long one still fit".
///
/// The name in the third comes from the current Windows user rather than being hardcoded. A fixed
/// name would be somebody's handle committed into a public repository, and a generic "Player" is
/// shorter than a real one and would under-represent the width.
/// </summary>
internal sealed class MessagePreview : Panel
{
    /// <summary>How far down the embedded frame this preview starts. See OnPaint for why not 0.</summary>
    private const int MessageGroundBand = 226;

    private int _chosen;
    private int _fpsSize = HudPrefs.ReadoutDefault;
    private (int Width, int Height)? _display;
    private double _scaler = HudPrefs.LanguageDefaultScaler;

    public MessagePreview()
    {
        DoubleBuffered = true;
        BackColor = HudGround.Fallback;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
    }

    public int Chosen
    {
        get => _chosen;
        set { _chosen = value; Invalidate(); }
    }

    /// <summary>
    /// The frame-rate readout's size, because it is the message list's NEIGHBOUR on screen and not a
    /// decoration here.
    ///
    /// The readout row sits at the HUD anchor, three pixels in and one above the top edge, and the
    /// message list starts at (10, 10) (InGameUI.cpp:1264-1265) - so in the corner of a real screen
    /// these two blocks are within a few pixels of each other, over the same terrain, and a player
    /// choosing a message size is choosing it next to that. Showing one alone was showing half the
    /// corner.
    /// </summary>
    public int FpsSize
    {
        get => _fpsSize;
        set { _fpsSize = value; Invalidate(); }
    }

    public (int Width, int Height)? Display
    {
        get => _display;
        set { _display = value; Invalidate(); }
    }

    public double Scaler
    {
        get => _scaler;
        set { _scaler = value; Invalidate(); }
    }

    /// <summary>
    /// 100 when the sample is drawn at true size, less when it had to be shrunk to fit this box.
    /// Read by the caption so that a shrunken preview never passes itself off as actual size.
    /// </summary>
    public int FitPercent { get; private set; } = 100;

    /// <summary>
    /// Raised when FitPercent changes, which can only be known during a paint.
    ///
    /// WITHOUT THIS THE CAPTION LIES. It is written when a control changes, the fit is discovered
    /// later when the repaint happens, and the caption would therefore still be claiming "actual
    /// size" underneath a sample that had just been shrunk to a third of it - which is exactly the
    /// confident-but-wrong preview this whole control exists to avoid.
    ///
    /// Raised only on a CHANGE, so the handler's label update cannot feed back into another paint
    /// of this control and loop.
    /// </summary>
    public event Action? FitChanged;

    private static string[] Samples()
    {
        var who = Environment.UserName;
        if (string.IsNullOrWhiteSpace(who))
        {
            who = "Player";
        }
        return new[]
        {
            "Unit under attack",
            "Upgrade Complete: AP Bullets",
            who + "-PC has been Defeated",
        };
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        Invalidate();            // the fit factor depends on the width LayoutRows just assigned
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;

        // A REAL FRAME OF THE GAME, NOT A DARK RECTANGLE, and the reason is in HudGround's header:
        // white text on flat black is legible at every size a person could type, so a preview on
        // black answered "yes, that is readable" to every question it was ever asked.
        //
        // THE BAND IS CHOSEN, NOT ZERO. The top of that frame is even desert - which is a fair
        // backdrop and an easy one, uniform enough that anything legible on part of it is legible on
        // all of it. This band puts a lit building and its shadow under the text as well, so the
        // sample crosses bright and dark inside one line, which is the case a size actually has to
        // survive. 226 keeps a 150-pixel box inside the frame with nothing tiled.
        HudGround.Paint(g, new Rectangle(0, 0, Width, Height), MessageGroundBand);

        using (var edge = new Pen(LauncherTheme.Border))
        {
            g.DrawRectangle(edge, 0, 0, Width - 1, Height - 1);
        }

        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;

        var samples = Samples();

        // The size the engine will hand to the font library. With no resolution recorded there is
        // nothing honest to scale by, so the chosen size is drawn as-is and the caption says so.
        var chosen = _chosen > 0 ? _chosen : HudPrefs.EngineBuiltInMessageSize;
        var drawnPt = _display is { } d
            ? HudPrefs.AdjustFontSize(chosen, d.Width, d.Height, _scaler)
            : chosen;
        var px = Math.Max(1, HudPrefs.PointsToPixels(drawnPt));

        // The neighbour: same arithmetic, its own preference, and 0 really does remove it
        // (postWindowDraw, InGameUI.cpp:4053).
        var readoutPt = _display is { } d2
            ? HudPrefs.AdjustFontSize(_fpsSize, d2.Width, d2.Height, _scaler)
            : _fpsSize;
        var readoutPx = readoutPt > 0 ? Math.Max(1, HudPrefs.PointsToPixels(readoutPt)) : 0;

        const int pad = 14;
        const float rowGap = 8f;
        var boxW = Math.Max(40, Width - (pad * 2));
        var boxH = Math.Max(30, Height - (pad * 2));

        var face = HudInk.Face(HudPrefs.EngineBuiltInMessageFont);
        var readoutFace = HudInk.Face("Tahoma");

        // Measure at true size first, then shrink ONLY if it does not fit. Scaling to fill would
        // make every size look the same, which is the one thing this control must never do.
        //
        // ONE FACTOR ACROSS BOTH BLOCKS. The readout is here so its size can be compared with the
        // message size; shrinking them independently would destroy the only thing the comparison is
        // for.
        var wasFit = FitPercent;
        FitPercent = 100;
        var fit = 1f;
        using (var probe = new Font(face, px, FontStyle.Regular, GraphicsUnit.Pixel))
        {
            var needW = 0f;
            foreach (var s in samples)
            {
                needW = Math.Max(needW, HudInk.Measure(g, s, probe));
            }
            // The engine advances by exactly the font's line height and adds nothing
            // (postDraw, InGameUI.cpp:4107-4110). This used to add six pixels a line, which made a
            // size look airier here than it does in a match.
            var needH = probe.GetHeight(g) * samples.Length;

            if (readoutPx > 0)
            {
                using var readoutProbe = new Font(readoutFace, readoutPx, FontStyle.Bold, GraphicsUnit.Pixel);
                needW = Math.Max(needW, ReadoutWidth(g, readoutProbe, 1f));
                needH += readoutProbe.GetHeight(g) + rowGap;
            }

            fit = Math.Min(needW > 0 ? boxW / needW : 1f, needH > 0 ? boxH / needH : 1f);
            if (fit < 1f)
            {
                FitPercent = Math.Max(1, (int)Math.Floor(fit * 100));
                px = Math.Max(1, (int)Math.Floor(px * fit));
                readoutPx = readoutPx > 0 ? Math.Max(1, (int)Math.Floor(readoutPx * fit)) : 0;
            }
            else
            {
                fit = 1f;
            }
        }
        if (FitPercent != wasFit)
        {
            FitChanged?.Invoke();
        }

        var y = (float)pad;

        if (readoutPx > 0)
        {
            using var readoutFont = new Font(readoutFace, readoutPx, FontStyle.Bold, GraphicsUnit.Pixel);
            var x = (float)pad;
            var gap = HudInk.GapPx * fit;
            x += HudInk.Draw(g, "59", readoutFont, HudInk.FpsInk, x, y);
            x += HudInk.Draw(g, "[60]", readoutFont, HudInk.FpsLimitInk, x, y) + gap;
            HudInk.Draw(g, "60tps", readoutFont, HudInk.LogicRateGood, x, y);
            y += readoutFont.GetHeight(g) + rowGap;
        }

        using var font = new Font(face, px, FontStyle.Regular, GraphicsUnit.Pixel);
        var step = font.GetHeight(g);

        // THE LIST ALTERNATES WHITE AND GREY, which it did not here and which is the half of the
        // question that was missing. addMessageText hands each new line m_messageColor1 or
        // m_messageColor2 depending on what the line above it got (InGameUI.cpp:2694-2697), so a real
        // list is white, (180,180,180), white - and the grey one is the line that gets hard to read
        // over bright ground first. A preview that drew all three in the same near-white was
        // answering an easier question than the game asks.
        for (var i = 0; i < samples.Length; i++)
        {
            var ink = (i % 2) == 0 ? HudInk.MessageInk1 : HudInk.MessageInk2;
            HudInk.Draw(g, samples[i], font, ink, pad, y);
            y += step;
        }
    }

    /// <summary>
    /// How wide the readout row wants to be, for the fit pass. The paint below walks the same three
    /// strings in the same order and does not need the total, which is why this is not shared with
    /// it - but it must stay in step with that walk or the block would be fitted to a width nothing
    /// was drawn at.
    /// </summary>
    private static float ReadoutWidth(Graphics g, Font font, float fit) =>
        HudInk.Measure(g, "59", font) +
        HudInk.Measure(g, "[60]", font) + (HudInk.GapPx * fit) +
        HudInk.Measure(g, "60tps", font);
}
