#include "flake.hh"
#include "lockfile.hh"
#include "primops.hh"
#include "eval-inline.hh"
#include "primops/fetchGit.hh"
#include "download.hh"
#include "args.hh"

#include <iostream>
#include <queue>
#include <regex>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace nix {

using namespace flake;

namespace flake {

/* Read a registry. */
std::shared_ptr<FlakeRegistry> readRegistry(const Path & path)
{
    auto registry = std::make_shared<FlakeRegistry>();

    if (!pathExists(path))
        return std::make_shared<FlakeRegistry>();

    auto json = nlohmann::json::parse(readFile(path));

    auto version = json.value("version", 0);
    if (version != 1)
        throw Error("flake registry '%s' has unsupported version %d", path, version);

    auto flakes = json["flakes"];
    for (auto i = flakes.begin(); i != flakes.end(); ++i)
        registry->entries.emplace(i.key(), FlakeRef(i->value("uri", "")));

    return registry;
}

/* Write a registry to a file. */
void writeRegistry(const FlakeRegistry & registry, const Path & path)
{
    nlohmann::json json;
    json["version"] = 1;
    for (auto elem : registry.entries)
        json["flakes"][elem.first.to_string()] = { {"uri", elem.second.to_string()} };
    createDirs(dirOf(path));
    writeFile(path, json.dump(4)); // The '4' is the number of spaces used in the indentation in the json file.
}

Path getUserRegistryPath()
{
    return getHome() + "/.config/nix/registry.json";
}

std::shared_ptr<FlakeRegistry> getUserRegistry()
{
    return readRegistry(getUserRegistryPath());
}

std::shared_ptr<FlakeRegistry> getFlagRegistry(RegistryOverrides registryOverrides)
{
    auto flagRegistry = std::make_shared<FlakeRegistry>();
    for (auto const & x : registryOverrides) {
        flagRegistry->entries.insert_or_assign(FlakeRef(x.first), FlakeRef(x.second));
    }
    return flagRegistry;
}

static FlakeRef lookupFlake(EvalState & state, const FlakeRef & flakeRef, const Registries & registries,
    std::vector<FlakeRef> pastSearches = {});

FlakeRef updateFlakeRef(EvalState & state, const FlakeRef & newRef, const Registries & registries, std::vector<FlakeRef> pastSearches)
{
    std::string errorMsg = "found cycle in flake registries: ";
    for (FlakeRef oldRef : pastSearches) {
        errorMsg += oldRef.to_string();
        if (oldRef == newRef)
            throw Error(errorMsg);
        errorMsg += " - ";
    }
    pastSearches.push_back(newRef);
    return lookupFlake(state, newRef, registries, pastSearches);
}

static FlakeRef lookupFlake(EvalState & state, const FlakeRef & flakeRef, const Registries & registries,
    std::vector<FlakeRef> pastSearches)
{
    for (std::shared_ptr<FlakeRegistry> registry : registries) {
        auto i = registry->entries.find(flakeRef);
        if (i != registry->entries.end()) {
            auto newRef = i->second;
            return updateFlakeRef(state, newRef, registries, pastSearches);
        }

        auto j = registry->entries.find(flakeRef.baseRef());
        if (j != registry->entries.end()) {
            auto newRef = j->second;
            newRef.ref = flakeRef.ref;
            newRef.rev = flakeRef.rev;
            newRef.subdir = flakeRef.subdir;
            return updateFlakeRef(state, newRef, registries, pastSearches);
        }
    }

    if (!flakeRef.isDirect())
        throw Error("could not resolve flake reference '%s'", flakeRef);

    return flakeRef;
}

FlakeRef maybeLookupFlake(
    EvalState & state,
    const FlakeRef & flakeRef,
    bool allowLookup)
{
    if (!flakeRef.isDirect()) {
        if (allowLookup)
            return lookupFlake(state, flakeRef, state.getFlakeRegistries());
        else
            throw Error("'%s' is an indirect flake reference, but registry lookups are not allowed", flakeRef);
    } else
        return flakeRef;
}


static SourceInfo fetchFlake(EvalState & state, const FlakeRef & resolvedRef)
{
    assert(resolvedRef.isDirect());

    auto doGit = [&](const GitInfo & gitInfo) {
        FlakeRef ref(resolvedRef.baseRef());
        ref.ref = gitInfo.ref;
        ref.rev = gitInfo.rev;
        SourceInfo info(ref);
        info.storePath = gitInfo.storePath;
        info.revCount = gitInfo.revCount;
        info.narHash = state.store->queryPathInfo(info.storePath)->narHash;
        info.lastModified = gitInfo.lastModified;
        return info;
    };

    // This only downloads only one revision of the repo, not the entire history.
    if (auto refData = std::get_if<FlakeRef::IsGitHub>(&resolvedRef.data)) {

        // FIXME: use regular /archive URLs instead? api.github.com
        // might have stricter rate limits.

        auto url = fmt("https://api.github.com/repos/%s/%s/tarball/%s",
            refData->owner, refData->repo,
            resolvedRef.rev ? resolvedRef.rev->to_string(Base16, false)
                : resolvedRef.ref ? *resolvedRef.ref : "master");

        std::string accessToken = settings.githubAccessToken.get();
        if (accessToken != "")
            url += "?access_token=" + accessToken;

        CachedDownloadRequest request(url);
        request.unpack = true;
        request.name = "source";
        request.ttl = resolvedRef.rev ? 1000000000 : settings.tarballTtl;
        request.getLastModified = true;
        auto result = getDownloader()->downloadCached(state.store, request);

        if (!result.etag)
            throw Error("did not receive an ETag header from '%s'", url);

        if (result.etag->size() != 42 || (*result.etag)[0] != '"' || (*result.etag)[41] != '"')
            throw Error("ETag header '%s' from '%s' is not a Git revision", *result.etag, url);

        FlakeRef ref(resolvedRef.baseRef());
        ref.rev = Hash(std::string(*result.etag, 1, result.etag->size() - 2), htSHA1);
        SourceInfo info(ref);
        info.storePath = result.storePath;
        info.narHash = state.store->queryPathInfo(info.storePath)->narHash;
        info.lastModified = result.lastModified;

        return info;
    }

    // This downloads the entire git history
    else if (auto refData = std::get_if<FlakeRef::IsGit>(&resolvedRef.data)) {
        return doGit(exportGit(state.store, refData->uri, resolvedRef.ref, resolvedRef.rev, "source"));
    }

    else if (auto refData = std::get_if<FlakeRef::IsPath>(&resolvedRef.data)) {
        if (!pathExists(refData->path + "/.git"))
            throw Error("flake '%s' does not reference a Git repository", refData->path);
        return doGit(exportGit(state.store, refData->path, {}, {}, "source"));
    }

    else abort();
}

Flake getFlake(EvalState & state, const FlakeRef & flakeRef)
{
    SourceInfo sourceInfo = fetchFlake(state, flakeRef);
    debug("got flake source '%s' with flakeref %s", sourceInfo.storePath, sourceInfo.resolvedRef.to_string());

    FlakeRef resolvedRef = sourceInfo.resolvedRef;

    state.store->assertStorePath(sourceInfo.storePath);

    if (state.allowedPaths)
        state.allowedPaths->insert(state.store->toRealPath(sourceInfo.storePath));

    // Guard against symlink attacks.
    Path flakeFile = canonPath(sourceInfo.storePath + "/" + resolvedRef.subdir + "/flake.nix");
    Path realFlakeFile = state.store->toRealPath(flakeFile);
    if (!isInDir(realFlakeFile, state.store->toRealPath(sourceInfo.storePath)))
        throw Error("'flake.nix' file of flake '%s' escapes from '%s'", resolvedRef, sourceInfo.storePath);

    Flake flake(flakeRef, sourceInfo);

    if (!pathExists(realFlakeFile))
        throw Error("source tree referenced by '%s' does not contain a '%s/flake.nix' file", resolvedRef, resolvedRef.subdir);

    Value vInfo;
    state.evalFile(realFlakeFile, vInfo); // FIXME: symlink attack

    state.forceAttrs(vInfo);

    auto sEdition = state.symbols.create("edition");
    auto sEpoch = state.symbols.create("epoch"); // FIXME: remove soon

    auto edition = vInfo.attrs->get(sEdition);
    if (!edition)
        edition = vInfo.attrs->get(sEpoch);

    if (edition) {
        flake.edition = state.forceInt(*(**edition).value, *(**edition).pos);
        if (flake.edition > 201909)
            throw Error("flake '%s' requires unsupported edition %d; please upgrade Nix", flakeRef, flake.edition);
        if (flake.edition < 201909)
            throw Error("flake '%s' has illegal edition %d", flakeRef, flake.edition);
    } else
        throw Error("flake '%s' lacks attribute 'edition'", flakeRef);

    if (auto description = vInfo.attrs->get(state.sDescription))
        flake.description = state.forceStringNoCtx(*(**description).value, *(**description).pos);

    auto sInputs = state.symbols.create("inputs");
    auto sUri = state.symbols.create("uri");
    auto sFlake = state.symbols.create("flake");

    if (std::optional<Attr *> inputs = vInfo.attrs->get(sInputs)) {
        state.forceAttrs(*(**inputs).value, *(**inputs).pos);

        for (Attr inputAttr : *(*(**inputs).value).attrs) {
            state.forceAttrs(*inputAttr.value, *inputAttr.pos);

            FlakeInput input(FlakeRef(inputAttr.name));

            for (Attr attr : *(inputAttr.value->attrs)) {
                if (attr.name == sUri) {
                    input.ref = state.forceStringNoCtx(*attr.value, *attr.pos);
                } else if (attr.name == sFlake) {
                    input.isFlake = state.forceBool(*attr.value, *attr.pos);
                } else
                    throw Error("flake input '%s' has an unsupported attribute '%s', at %s",
                        inputAttr.name, attr.name, *attr.pos);
            }

            flake.inputs.emplace(inputAttr.name, input);
        }
    }

    auto sOutputs = state.symbols.create("outputs");

    if (auto outputs = vInfo.attrs->get(sOutputs)) {
        state.forceFunction(*(**outputs).value, *(**outputs).pos);
        flake.vOutputs = (**outputs).value;

        if (flake.vOutputs->lambda.fun->matchAttrs) {
            for (auto & formal : flake.vOutputs->lambda.fun->formals->formals) {
                if (formal.name != state.sSelf)
                    flake.inputs.emplace(formal.name, FlakeInput(FlakeRef(formal.name)));
            }
        }

    } else
        throw Error("flake '%s' lacks attribute 'outputs'", flakeRef);

    for (auto & attr : *vInfo.attrs) {
        if (attr.name != sEdition &&
            attr.name != sEpoch &&
            attr.name != state.sDescription &&
            attr.name != sInputs &&
            attr.name != sOutputs)
            throw Error("flake '%s' has an unsupported attribute '%s', at %s",
                flakeRef, attr.name, *attr.pos);
    }

    return flake;
}

static SourceInfo getNonFlake(EvalState & state, const FlakeRef & flakeRef)
{
    auto sourceInfo = fetchFlake(state, flakeRef);
    debug("got non-flake source '%s' with flakeref %s", sourceInfo.storePath, sourceInfo.resolvedRef.to_string());

    FlakeRef resolvedRef = sourceInfo.resolvedRef;

    state.store->assertStorePath(sourceInfo.storePath);

    if (state.allowedPaths)
        state.allowedPaths->insert(sourceInfo.storePath);

    return sourceInfo;
}

bool allowedToWrite(HandleLockFile handle)
{
    return handle == UpdateLockFile || handle == RecreateLockFile;
}

bool recreateLockFile(HandleLockFile handle)
{
    return handle == RecreateLockFile || handle == UseNewLockFile;
}

bool allowedToUseRegistries(HandleLockFile handle, bool isTopRef)
{
    if (handle == AllPure) return false;
    else if (handle == TopRefUsesRegistries) return isTopRef;
    else if (handle == UpdateLockFile) return true;
    else if (handle == UseUpdatedLockFile) return true;
    else if (handle == RecreateLockFile) return true;
    else if (handle == UseNewLockFile) return true;
    else assert(false);
}

/* Given a flakeref and its subtree of the lockfile, return an updated
   subtree of the lockfile. That is, if the 'flake.nix' of the
   referenced flake has inputs that don't have a corresponding entry
   in the lockfile, they're added to the lockfile; conversely, any
   lockfile entries that don't have a corresponding entry in flake.nix
   are removed.

   Note that this is lazy: we only recursively fetch inputs that are
   not in the lockfile yet. */
static std::pair<Flake, LockedInput> updateLocks(
    EvalState & state,
    const Flake & flake,
    HandleLockFile handleLockFile,
    const LockedInputs & oldEntry,
    bool topRef)
{
    LockedInput newEntry(
        flake.sourceInfo.resolvedRef,
        flake.sourceInfo.narHash);

    for (auto & [id, input] : flake.inputs) {
        auto i = oldEntry.inputs.find(id);
        if (i != oldEntry.inputs.end()) {
            newEntry.inputs.insert_or_assign(id, i->second);
        } else {
            if (handleLockFile == AllPure || handleLockFile == TopRefUsesRegistries)
                throw Error("cannot update flake input '%s' in pure mode", id);
            if (input.isFlake)
                newEntry.inputs.insert_or_assign(id,
                    updateLocks(state,
                        getFlake(state, maybeLookupFlake(state, input.ref, allowedToUseRegistries(handleLockFile, false))),
                        handleLockFile, {}, false).second);
            else {
                auto sourceInfo = getNonFlake(state, maybeLookupFlake(state, input.ref, allowedToUseRegistries(handleLockFile, false)));
                newEntry.inputs.insert_or_assign(id, LockedInput(sourceInfo.resolvedRef, sourceInfo.narHash));
            }
        }
    }

    return {flake, newEntry};
}

/* Compute an in-memory lockfile for the specified top-level flake,
   and optionally write it to file, it the flake is writable. */
ResolvedFlake resolveFlake(EvalState & state, const FlakeRef & topRef, HandleLockFile handleLockFile)
{
    auto flake = getFlake(state, maybeLookupFlake(state, topRef, allowedToUseRegistries(handleLockFile, true)));

    LockFile oldLockFile;

    if (!recreateLockFile(handleLockFile)) {
        // If recreateLockFile, start with an empty lockfile
        // FIXME: symlink attack
        oldLockFile = LockFile::read(
            state.store->toRealPath(flake.sourceInfo.storePath)
            + "/" + flake.sourceInfo.resolvedRef.subdir + "/flake.lock");
    }

    LockFile lockFile(updateLocks(
            state, flake, handleLockFile, oldLockFile, true).second);

    if (!(lockFile == oldLockFile)) {
        if (allowedToWrite(handleLockFile)) {
            if (auto refData = std::get_if<FlakeRef::IsPath>(&topRef.data)) {
                if (lockFile.isDirty())
                    warn("will not write lock file of flake '%s' because it has a dirty input", topRef);
                else {
                    lockFile.write(refData->path + (topRef.subdir == "" ? "" : "/" + topRef.subdir) + "/flake.lock");

                    // Hack: Make sure that flake.lock is visible to Git, so it ends up in the Nix store.
                    runProgram("git", true,
                        { "-C", refData->path, "add",
                          "--force",
                          "--intent-to-add",
                          (topRef.subdir == "" ? "" : topRef.subdir + "/") + "flake.lock" });
                }
            } else
                warn("cannot write lock file of remote flake '%s'", topRef);
        } else if (handleLockFile != AllPure && handleLockFile != TopRefUsesRegistries)
            warn("using updated lock file without writing it to file");
    }

    return ResolvedFlake(std::move(flake), std::move(lockFile));
}

void updateLockFile(EvalState & state, const FlakeRef & flakeRef, bool recreateLockFile)
{
    resolveFlake(state, flakeRef, recreateLockFile ? RecreateLockFile : UpdateLockFile);
}

static void emitSourceInfoAttrs(EvalState & state, const SourceInfo & sourceInfo, Value & vAttrs)
{
    auto & path = sourceInfo.storePath;
    assert(state.store->isValidPath(path));
    mkString(*state.allocAttr(vAttrs, state.sOutPath), path, {path});

    if (sourceInfo.resolvedRef.rev) {
        mkString(*state.allocAttr(vAttrs, state.symbols.create("rev")),
            sourceInfo.resolvedRef.rev->gitRev());
        mkString(*state.allocAttr(vAttrs, state.symbols.create("shortRev")),
            sourceInfo.resolvedRef.rev->gitShortRev());
    }

    if (sourceInfo.revCount)
        mkInt(*state.allocAttr(vAttrs, state.symbols.create("revCount")), *sourceInfo.revCount);

    if (sourceInfo.lastModified)
        mkString(*state.allocAttr(vAttrs, state.symbols.create("lastModified")),
            fmt("%s",
                std::put_time(std::gmtime(&*sourceInfo.lastModified), "%Y%m%d%H%M%S")));
}

struct LazyInput
{
    bool isFlake;
    LockedInput lockedInput;
};

/* Helper primop to make callFlake (below) fetch/call its inputs
   lazily. Note that this primop cannot be called by user code since
   it doesn't appear in 'builtins'. */
static void prim_callFlake(EvalState & state, const Pos & pos, Value * * args, Value & v)
{
    auto lazyInput = (LazyInput *) args[0]->attrs;

    assert(lazyInput->lockedInput.ref.isImmutable());

    if (lazyInput->isFlake) {
        auto flake = getFlake(state, lazyInput->lockedInput.ref);

        if (flake.sourceInfo.narHash != lazyInput->lockedInput.narHash)
            throw Error("the content hash of flake '%s' doesn't match the hash recorded in the referring lockfile", flake.sourceInfo.resolvedRef);

        callFlake(state, flake, lazyInput->lockedInput, v);
    } else {
        auto sourceInfo = getNonFlake(state, lazyInput->lockedInput.ref);

        if (sourceInfo.narHash != lazyInput->lockedInput.narHash)
            throw Error("the content hash of repository '%s' doesn't match the hash recorded in the referring lockfile", sourceInfo.resolvedRef);

        state.mkAttrs(v, 8);

        assert(state.store->isValidPath(sourceInfo.storePath));

        mkString(*state.allocAttr(v, state.sOutPath),
            sourceInfo.storePath, {sourceInfo.storePath});

        emitSourceInfoAttrs(state, sourceInfo, v);
    }
}

void callFlake(EvalState & state,
    const Flake & flake,
    const LockedInputs & lockedInputs,
    Value & vRes)
{
    auto & vInputs = *state.allocValue();

    state.mkAttrs(vInputs, flake.inputs.size() + 1);

    for (auto & [inputId, input] : flake.inputs) {
        auto vFlake = state.allocAttr(vInputs, inputId);
        auto vPrimOp = state.allocValue();
        static auto primOp = new PrimOp(prim_callFlake, 1, state.symbols.create("callFlake"));
        vPrimOp->type = tPrimOp;
        vPrimOp->primOp = primOp;
        auto vArg = state.allocValue();
        vArg->type = tNull;
        auto lockedInput = lockedInputs.inputs.find(inputId);
        assert(lockedInput != lockedInputs.inputs.end());
        // FIXME: leak
        vArg->attrs = (Bindings *) new LazyInput{input.isFlake, lockedInput->second};
        mkApp(*vFlake, *vPrimOp, *vArg);
    }

    auto & vSourceInfo = *state.allocValue();
    state.mkAttrs(vSourceInfo, 8);
    emitSourceInfoAttrs(state, flake.sourceInfo, vSourceInfo);

    vInputs.attrs->push_back(Attr(state.sSelf, &vRes));

    vInputs.attrs->sort();

    /* For convenience, put the outputs directly in the result, so you
       can refer to an output of an input as 'inputs.foo.bar' rather
       than 'inputs.foo.outputs.bar'. */
    auto vCall = *state.allocValue();
    state.eval(state.parseExprFromString(
            "outputsFun: inputs: sourceInfo: let outputs = outputsFun inputs; in "
            "outputs // sourceInfo // { inherit inputs; inherit outputs; inherit sourceInfo; }", "/"), vCall);

    auto vCall2 = *state.allocValue();
    auto vCall3 = *state.allocValue();
    state.callFunction(vCall, *flake.vOutputs, vCall2, noPos);
    state.callFunction(vCall2, vInputs, vCall3, noPos);
    state.callFunction(vCall3, vSourceInfo, vRes, noPos);
}

void callFlake(EvalState & state,
    const ResolvedFlake & resFlake,
    Value & v)
{
    callFlake(state, resFlake.flake, resFlake.lockFile, v);
}

// This function is exposed to be used in nix files.
static void prim_getFlake(EvalState & state, const Pos & pos, Value * * args, Value & v)
{
    callFlake(state, resolveFlake(state, state.forceStringNoCtx(*args[0], pos),
            evalSettings.pureEval ? AllPure : UseUpdatedLockFile), v);
}

static RegisterPrimOp r2("getFlake", 1, prim_getFlake);

void gitCloneFlake(FlakeRef flakeRef, EvalState & state, Registries registries, const Path & destDir)
{
    flakeRef = lookupFlake(state, flakeRef, registries);

    std::string uri;

    Strings args = {"clone"};

    if (auto refData = std::get_if<FlakeRef::IsGitHub>(&flakeRef.data)) {
        uri = "git@github.com:" + refData->owner + "/" + refData->repo + ".git";
        args.push_back(uri);
        if (flakeRef.ref) {
            args.push_back("--branch");
            args.push_back(*flakeRef.ref);
        }
    } else if (auto refData = std::get_if<FlakeRef::IsGit>(&flakeRef.data)) {
        args.push_back(refData->uri);
        if (flakeRef.ref) {
            args.push_back("--branch");
            args.push_back(*flakeRef.ref);
        }
    }

    if (destDir != "")
        args.push_back(destDir);

    runProgram("git", true, args);
}

}

std::shared_ptr<flake::FlakeRegistry> EvalState::getGlobalFlakeRegistry()
{
    std::call_once(_globalFlakeRegistryInit, [&]() {
        auto path = evalSettings.flakeRegistry;

        if (!hasPrefix(path, "/")) {
            CachedDownloadRequest request(evalSettings.flakeRegistry);
            request.name = "flake-registry.json";
            request.gcRoot = true;
            path = getDownloader()->downloadCached(store, request).path;
        }

        _globalFlakeRegistry = readRegistry(path);
    });

    return _globalFlakeRegistry;
}

// This always returns a vector with flakeReg, userReg, globalReg.
// If one of them doesn't exist, the registry is left empty but does exist.
const Registries EvalState::getFlakeRegistries()
{
    Registries registries;
    registries.push_back(getFlagRegistry(registryOverrides));
    registries.push_back(getUserRegistry());
    registries.push_back(getGlobalFlakeRegistry());
    return registries;
}

Fingerprint ResolvedFlake::getFingerprint() const
{
    // FIXME: as an optimization, if the flake contains a lock file
    // and we haven't changed it, then it's sufficient to use
    // flake.sourceInfo.storePath for the fingerprint.
    return hashString(htSHA256,
        fmt("%s;%s", flake.sourceInfo.storePath, lockFile));
}

}
