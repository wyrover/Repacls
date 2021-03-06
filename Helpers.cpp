#define UMDF_USING_NTSTATUS
#include <ntstatus.h>

#include <windows.h>
#include <sddl.h>
#include <lmcons.h>
#include <stdio.h>
#include <ntsecapi.h>
#include <atlbase.h>
#include <atlstr.h>
#include <wscapi.h>
#include <iwscapi.h>

#include <string>
#include <map>
#include <shared_mutex>

#include "Operation.h"
#include "Functions.h"

typedef struct SidCompare
{
	inline bool operator()(PSID p1, PSID p2) const
	{
		const DWORD iLength1 = GetLengthSid(p1);
		const DWORD iLength2 = GetLengthSid(p2);
		if (iLength1 != iLength2) return iLength1 < iLength2;
		return memcmp(p1, p2, iLength1) > 0;
	}
} 
SidCompare;

const PSID GetSidFromName(std::wstring & sAccountName)
{
	// for caching
	static std::shared_mutex oMutex;
	static std::map<std::wstring, PSID> oNameToSidLookup;

	// scope lock for thread safety
	{
		std::shared_lock<std::shared_mutex> oLock(oMutex);
		std::map<std::wstring, PSID>::iterator oInteractor = oNameToSidLookup.find(sAccountName);
		if (oInteractor != oNameToSidLookup.end())
		{
			return oInteractor->second;
		}
	}

	// first see if the name look like a sid
	PSID tSidFromSid;
	if (ConvertStringSidToSid(sAccountName.c_str(), &tSidFromSid) != 0)
	{
		return tSidFromSid;
	}

	// assume the sid is as large as it possible can be
	BYTE tSidFromName[SECURITY_MAX_SID_SIZE];
	WCHAR sDomainName[UNLEN + 1];
	DWORD iDomainName = _countof(sDomainName);
	DWORD iSidSize = sizeof(tSidFromName);
	SID_NAME_USE tNameUse;

	// do lookup
	if (LookupAccountName(NULL, sAccountName.c_str(), (PSID)tSidFromName,
		&iSidSize, sDomainName, &iDomainName, &tNameUse) == 0)
	{
		std::unique_lock<std::shared_mutex> oLock(oMutex);
		oNameToSidLookup[sAccountName] = nullptr;
		return nullptr;
	}

	// reallocate memory and copy sid to a smaller part of memory and
	// then add the sid to the cache map
	PSID tSid = (PSID)memcpy(malloc(iSidSize), tSidFromName, iSidSize);

	// scope lock for thread safety
	{
		std::unique_lock<std::shared_mutex> oLock(oMutex);
		oNameToSidLookup[sAccountName] = tSid;
	}

	// return the sid pointer to the caller
	return tSid;
}

std::wstring GetNameFromSid(const PSID tSid, bool * bMarkAsOrphan)
{
	// return immediately if sid is null
	if (tSid == NULL) return L"";

	// for caching
	static std::shared_mutex oMutex;
	static std::map<PSID, std::wstring, SidCompare> oSidToNameLookup;

	// scope lock for thread safety
	{
		std::shared_lock<std::shared_mutex> oLock(oMutex);
		std::map<PSID, std::wstring>::iterator oInteractor = oSidToNameLookup.find(tSid);
		if (oInteractor != oSidToNameLookup.end())
		{
			// if blank that means the account has no associated same
			// and likely is an orphan
			if (oInteractor->second == L"" &&
				bMarkAsOrphan != NULL) *bMarkAsOrphan = true;

			// return the found full name
			return oInteractor->second;
		}
	}

	// lookup the name for this sid
	SID_NAME_USE tNameUse;
	WCHAR sAccountName[UNLEN + 1];
	DWORD iAccountNameSize = _countof(sAccountName);
	WCHAR sDomainName[UNLEN + 1];
	DWORD iDomainName = _countof(sDomainName);
	std::wstring sFullName = L"";
	if (LookupAccountSid(NULL, tSid, sAccountName,
		&iAccountNameSize, sDomainName, &iDomainName, &tNameUse) == 0)
	{
		DWORD iError = GetLastError();
		if (iError == ERROR_NONE_MAPPED)
		{
			if (bMarkAsOrphan != NULL) *bMarkAsOrphan = true;
		}
		else
		{
			wprintf(L"ERROR: Unexpected error returned from account lookup (%lu).\n", iError);
			return L"";
		}
	}
	else
	{
		// generate full name in domain\name format
		sFullName = std::wstring(sDomainName) +
			L"\\" + std::wstring(sAccountName);
	}

	// copy the sid for storage in our cache table
	const DWORD iSidLength = GetLengthSid(tSid);
	PSID tSidCopy = (PSID)memcpy(malloc(iSidLength), tSid, iSidLength);

	// scope lock for thread safety
	{
		std::unique_lock<std::shared_mutex> oLock(oMutex);
		oSidToNameLookup[tSidCopy] = sFullName;
	}

	// return name
	return sFullName;
}

std::wstring GetNameFromSidEx(const PSID tSid, bool * bMarkAsOrphan)
{
	// if sid is resolvable then return the account name
	std::wstring sName = GetNameFromSid(tSid, bMarkAsOrphan);
	if (sName != L"") return sName;

	// if sid is unresolvable then return sid in string form
	WCHAR * sSidBuf;
	ConvertSidToStringSid(tSid, &sSidBuf);
	std::wstring sSid(sSidBuf);
	LocalFree(sSidBuf);
	return sSid;
}

std::wstring GetDomainNameFromSid(const PSID tSid)
{
	// do a reverse lookup using our normal call
	std::wstring sDomainName = GetNameFromSidEx(tSid, NULL);

	// sometimes the domain will be returned as DOMAIN\DOMAIN instead
	// of just DOMAIN\ so lets trim off any excess characters
	std::wstring::size_type nSlash = sDomainName.find(L"\\");
	if (nSlash != std::wstring::npos) sDomainName.erase(nSlash + 1);
	return sDomainName;
}

std::wstring GenerateInheritanceFlags(DWORD iCurrentFlags)
{
	std::wstring sFlags;
	if (CONTAINER_INHERIT_ACE & iCurrentFlags) sFlags += L"Container Inherit;";
	if (OBJECT_INHERIT_ACE & iCurrentFlags) sFlags += L"Object Inherit;";
	if (NO_PROPAGATE_INHERIT_ACE & iCurrentFlags) sFlags += L"Do No Propagate Inherit;";
	if (INHERIT_ONLY_ACE & iCurrentFlags) sFlags += L"Inherit Only;";

	// handle the empty case or trim off the trailing semicolon
	if (sFlags.size() == 0) sFlags = L"None";
	else sFlags.pop_back();

	// return the calculated string
	return sFlags;
}


std::wstring GenerateAccessMask(DWORD iCurrentMask)
{
	// define the aesthetic names of permission 
	static struct
	{
		const DWORD Mask;
		const std::wstring Description;
	}
	MaskDefinitions[] =
	{
		{ FILE_ALL_ACCESS, L"Full Control" },
		{ FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE, L"Modify" },
		{ FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, L"Read & Execute" },
		{ FILE_GENERIC_EXECUTE, L"Execute" },
		{ FILE_GENERIC_READ, L"Read" },
		{ FILE_GENERIC_WRITE, L"Write" },
		{ FILE_EXECUTE, L"Traverse Folder/Execute File" },
		{ FILE_READ_DATA, L"List Folder/Read Data" },
		{ FILE_READ_ATTRIBUTES, L"Read Attributes" },
		{ FILE_READ_EA, L"Read Extended Attributes" },
		{ FILE_WRITE_DATA, L"Create Files/Write Data" },
		{ FILE_APPEND_DATA, L"Create Folders/Append Data" },
		{ FILE_WRITE_ATTRIBUTES, L"Write Attributes" },
		{ FILE_WRITE_EA, L"Write Extended Attributes" },
		{ FILE_DELETE_CHILD, L"Delete Children" },
		{ DELETE, L"Delete" },
		{ READ_CONTROL, L"Read Permissions" },
		{ WRITE_DAC, L"Set Permissions" },
		{ WRITE_OWNER, L"Take Ownership" },
		{ SYNCHRONIZE, L"Synchronize" },
		{ GENERIC_ALL, L"Generic All" },
		{ GENERIC_WRITE, L"Generic Write" },
		{ GENERIC_READ, L"Generic Read" },
		{ GENERIC_EXECUTE, L"Generic Execute" }
	};

	// loop through the mask and construct of string of the names
	std::wstring sMaskList;
	for (int iMaskEntry = 0; iMaskEntry < _countof(MaskDefinitions) && iCurrentMask > 0; iMaskEntry++)
	{
		if ((MaskDefinitions[iMaskEntry].Mask & iCurrentMask) == MaskDefinitions[iMaskEntry].Mask)
		{
			sMaskList += MaskDefinitions[iMaskEntry].Description + L";";
			iCurrentMask ^= MaskDefinitions[iMaskEntry].Mask;
		}
	}

	// if any remaining permission bits exist, append them as well
	if (iCurrentMask != 0)
	{
		sMaskList += L"Unrecognized Permissions (" + std::to_wstring(iCurrentMask) + L");";
	}

	// handle the empty case or trim off the trailing semicolon
	if (sMaskList.size() == 0) sMaskList = L"None";
	else sMaskList.pop_back();

	// return the calculated string
	return sMaskList;
}

VOID EnablePrivs()
{
	// open the current token
	HANDLE hToken = NULL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &hToken) == 0)
	{
		// error
		wprintf(L"%s\n", L"ERROR: Could not open process token for enabling privileges.");
		return;
	}

	// get the current user sid out of the token
	BYTE aBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
	PTOKEN_USER tTokenUser = (PTOKEN_USER)(aBuffer);
	DWORD iBytesFilled = 0;
	if (GetTokenInformation(hToken, TokenUser, tTokenUser, sizeof(aBuffer), &iBytesFilled) == 0)
	{
		// error
		wprintf(L"%s\n", L"ERROR: Could retrieve process token information.");
		return;
	}

	WCHAR * sPrivsToSet[] = { SE_RESTORE_NAME, SE_BACKUP_NAME, SE_TAKE_OWNERSHIP_NAME, SE_SECURITY_NAME };
	for (int i = 0; i < sizeof(sPrivsToSet) / sizeof(WCHAR *); i++)
	{
		// populate the privilege adjustment structure
		TOKEN_PRIVILEGES tPrivEntry;
		tPrivEntry.PrivilegeCount = 1;
		tPrivEntry.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		// translate the privilege name into the binary representation
		if (LookupPrivilegeValue(NULL, sPrivsToSet[i],
			&tPrivEntry.Privileges[0].Luid) == 0)
		{
			wprintf(L"ERROR: Could not lookup privilege: %s\n", sPrivsToSet[i]);
			continue;
		}

		// adjust the process to change the privilege
		if (AdjustTokenPrivileges(hToken, FALSE, &tPrivEntry,
			sizeof(TOKEN_PRIVILEGES), NULL, NULL) != 0)
		{
			// enabling was successful
			continue;
		}

		// Object attributes are reserved, so initialize to zeros.
		LSA_OBJECT_ATTRIBUTES ObjectAttributes;
		ZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));

		// Get a handle to the Policy object.
		LSA_HANDLE hPolicyHandle;
		NTSTATUS iResult = 0;
		if ((iResult = LsaOpenPolicy(NULL, &ObjectAttributes,
			POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &hPolicyHandle)) != STATUS_SUCCESS)
		{
			wprintf(L"ERROR: Local security policy could not be opened with error '%lu'\n",
				LsaNtStatusToWinError(iResult));
			continue;
		}

		// convert the privilege name to a unicode string format
		LSA_UNICODE_STRING sPrivilege;
		sPrivilege.Buffer = sPrivsToSet[i];
		sPrivilege.Length = (USHORT)(wcslen(sPrivsToSet[i]) * sizeof(WCHAR));
		sPrivilege.MaximumLength = (USHORT)((wcslen(sPrivsToSet[i]) + 1) * sizeof(WCHAR));

		// attempt to add the account to policy
		if ((iResult = LsaAddAccountRights(hPolicyHandle,
			tTokenUser->User.Sid, &sPrivilege, 1)) != STATUS_SUCCESS)
		{
			LsaClose(hPolicyHandle);
			wprintf(L"ERROR: Privilege '%s' was not able to be added with error '%lu'\n",
				sPrivsToSet[i], LsaNtStatusToWinError(iResult));
			continue;
		}

		// cleanup
		LsaClose(hPolicyHandle);

		if (AdjustTokenPrivileges(hToken, FALSE, &tPrivEntry,
			sizeof(TOKEN_PRIVILEGES), NULL, NULL) == 0 || GetLastError() != ERROR_NOT_ALL_ASSIGNED)
		{
			wprintf(L"ERROR: Could not adjust privilege: %s\n", sPrivsToSet[i]);
			continue;
		}

		// error if not all items were assigned
		if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
		{
			wprintf(L"ERROR: Could not enable privilege: %s\n", sPrivsToSet[i]);
		}
	}

	CloseHandle(hToken);
	return;
}

HANDLE RegisterFileHandle(HANDLE hFile, std::wstring sOperation)
{
	// lookup do a reverse lookup on file name
	static std::map<std::wstring, std::pair<HANDLE,std::wstring>> oFileLookup;

	// do a reverse lookup on the file name
	DWORD iSize = GetFinalPathNameByHandle(hFile, NULL, 0, VOLUME_NAME_NT);

	// create a string that can accommodate that size (plus null terminating character)
	std::wstring sPath;
	sPath.resize(iSize + 1);

	// get the full name
	if (GetFinalPathNameByHandle(hFile, (LPWSTR)sPath.data(), (DWORD)sPath.capacity(), VOLUME_NAME_NT) == 0)
	{
		wprintf(L"ERROR: The true path to the specified file could not be determined.\n");
		exit(-1);
	}

	// resize string back to actual size to remove null terminating character from string data
	sPath.resize(iSize);

	// if the handle already exists, then use that one if the parameters match
	std::map<std::wstring, std::pair<HANDLE,std::wstring>>::iterator oFile = oFileLookup.find(sPath);
	if (oFile != oFileLookup.end())
	{
		if (oFileLookup[sPath].second == sOperation)
		{
			CloseHandle(hFile);
			return oFileLookup[sPath].first;
		}
		else
		{
			wprintf(L"ERROR: The same file was used in mismatching read/write operations.\n");
			exit(-1);
		}
	}
	else
	{
		oFileLookup[std::wstring(sPath)] = std::pair<HANDLE, std::wstring>(hFile, sOperation);
		return hFile;
	}
}

bool CheckIfAntivirusIsActive()
{
	// initialize COM for checking the antivirus status
	HRESULT hResult = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
	if (hResult != S_OK && hResult != S_FALSE)
	{
		return false;
	}

	// assume not installed by default
	bool bIsInstalled = false;

	// query the product list
	IWSCProductList * PtrProductList = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(WSCProductList), NULL, CLSCTX_INPROC_SERVER,
		__uuidof(IWSCProductList), reinterpret_cast<LPVOID*> (&PtrProductList))))
	{
		return false;
	}

	// initialize the antivirus provider list
	if (FAILED(PtrProductList->Initialize(WSC_SECURITY_PROVIDER_ANTIVIRUS)))
	{
		PtrProductList->Release();
		return false;
	}

	// get the current product count
	LONG ProductCount = 0;
	if (FAILED(PtrProductList->get_Count(&ProductCount)))
	{
		PtrProductList->Release();
		return false;
	}

	for (LONG i = 0; i < ProductCount; i++)
	{
		// get the product details
		IWscProduct * PtrProduct = nullptr;
		if (FAILED(PtrProductList->get_Item(i, &PtrProduct)))
		{
			PtrProductList->Release();
			return false;
		}

		// fetch the product state
		WSC_SECURITY_PRODUCT_STATE ProductState;
		if (FAILED(PtrProduct->get_ProductState(&ProductState)))
		{
			PtrProduct->Release();
			PtrProductList->Release();
			return false;
		}

		bIsInstalled |= (ProductState == WSC_SECURITY_PRODUCT_STATE_ON);
		PtrProduct->Release();
	}

	// return status
	PtrProductList->Release();
	return bIsInstalled;
}