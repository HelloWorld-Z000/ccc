#pragma once

namespace SD::Compat
{
	// Logs, by DLL name, the mods that stop Scene Director from working, at
	// startup rather than when the first conversation fails. (SmoothCam only
	// reports an owner handle, which means nothing to a reader.)
	void ReportKnownConflicts();
}
