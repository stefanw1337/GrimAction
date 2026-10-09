# Cursor test before release

Release 0.2.3 requires in-game acceptance on the supported patch.

1. Fully exit Grim Dawn if another GrimAction build was loaded. Do not start two builds in the same game session.
2. Start Grim Dawn in DirectX 11 and wait for the main menu.
3. Run this package's **Start GrimAction.cmd** once. Keep the runtime DLL in this package, outside the game folder.
4. Enter the world. F8 on should hide the cursor completely, with no dot. F8 off should restore the normal cursor.
5. Open Escape, inventory, map and an NPC conversation while F8 is on. The cursor should appear and allow clicking. It should hide again when camera control resumes.
6. Hold Alt to show the cursor; release Alt to hide it. Also test switching away from the game and back.
7. Toggle F8 several times. Test **Stop GrimAction.cmd** while the cursor is hidden.

If something fails, record the step and keep the CSV file in logs. A cursor_visibility value of 1 means that hiding was attempted on the expected thread; it does not prove the visual result. Fully exiting the game removes the loaded runtime. An older GrimAction package will only work with its matching game patch.

Known limitations: DX9 is unchanged. Detection of devotion, blacksmith, rift gates, death screens and other panels is not fully verified. Alt releases the cursor. Test these panels before treating the build as accepted. Fully restart the game after world changes if camera controls stop responding.
