# Genral Features

Todo as features are implemented mark as Agents work code, mark done only when tested by human.


## PWD for cli terms in different side tabs 

When opening the CLI tab typically there is already one tab open from boot that is in the wrong directory, typically use home.
When opening a project we should have... 
In Terminal side tab, then single term instance shold cd to project root, provided that no command is currently running when the project open occurs
In the CLI tab, if there is a terminal with the tab "Term 1",  i.e. user has not manipulated the default tab, and this tabs terminal is not running a command
 it should cd to the new project root when a project is opened.

The agent tab is more complicated.  Setting the directory in running claude is impossible, detecting it is safe to exit may be impossible but ideally...
If the Agent is running in the wrong directory, nothing is executing now, no text has been entered, and the context window is empty so we are sure no work is lost
 we should exit the claude instance by sending  /exit [enter], it will automatically restart in the correct directory.
 exiting OpenCode may be different commands, and detecting its safe will likey be different too.

Need to investigate close integration with Claude and the IDE generally currently we just run cli app in the correct directory.
What are the options for CLaude integration what data can we get from the running lcaude instance.
