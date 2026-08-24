# Genral Features

Todo as features are implemented mark as Agents work code, mark done only when tested by human.

ctrl+shift up|down arrow should move a line of text up in the editor

# Focus improvements

DONE When clicking on a lower tab (e.g. CLI, the open terminal should get focus)

DONE Progress when opening a plan or progress doc it should get focus

- When running a file tool from the tree browser menu if the doc is open it should be refreshed. [TECHNICAL CONCERN: file tools run async in terminal; no hook to detect completion for reload]
- When running a build from the tree browser drop down, and output is configured to go to a particular tab that tab should get focus. [TECHNICAL CONCERN: async build, no completion signal from treebrowser toolbar tools]
- DONE When clicking the heart (favourite/project root) in tree browser the tree should get focus
  - DONE Same for home button.
  - DONE and refresh button
- DONE When expanding a tree item the first item in the just expanded tree should get focus
- DONE When geany agent creates a new prompt the text editor should get focus
- DONE When switching agent the agent cli should get focus
- DONE When creating a new folder via the treebrowsers `.` virtual folder the new folder should get focus
- DONE When tree browser "track path" is clicked, the tree item is selected it should get focus too.
- DONE If you click "track path", and the file in the editor is an existing file but not in the tree view yet, refresh the directories needed because the file does exist!
- DONE Ctrl+Click on track path should navigate and copy the relative path of the file too
- DONE If you click refresh on a closed tree item the tree item should open and the first in the expanded tree should get focus
- If a directory has focus in the tree browser when you select "Find in files" that directory should be prepopulated as the place to search [TECHNICAL CONCERN: right-click context menu already does this; hooking Geany's main-menu Find in Files requires Geany API not available to plugins]
- DONE If you copy a file and then paste it into the same directory it should ask for a new name so not to overwrite the file with itself
