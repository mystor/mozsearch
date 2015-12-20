/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/AST/Mangle.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <map>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>

// Needed for sha1 hacks
#include <fcntl.h>
#include <unistd.h>
#include "sha1.h"

using namespace clang;

const std::string GENERATED("__GENERATED__/");

std::string srcdir;
std::string outdir;

static std::string
GetMangledName(clang::MangleContext* ctx,
               const clang::NamedDecl* decl)
{
  llvm::SmallVector<char, 512> output;
  llvm::raw_svector_ostream out(output);
  ctx->mangleName(decl, out);
  return out.str().str();
}

// BEWARE: use only as a temporary
const char *
hash(std::string &str)
{
  static unsigned char rawhash[20];
  static char hashstr[41];
  sha1::calc(str.c_str(), str.size(), rawhash);
  sha1::toHexString(rawhash, hashstr);
  return hashstr;
}

std::string
EscapeString(std::string input)
{
  std::string output = "\"";
  char hex[] = { '0', '1', '2', '3', '4', '5', '6', '7',
                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
  for (char c : input) {
    if (isspace(c) || c == '"' || c == '\\') {
      output += "\\x";
      output += hex[c >> 4];
      output += hex[c & 0xf];
    } else {
      output += c;
    }
  }
  output += '"';
  return output;
}

std::string
ToString(int n)
{
  char s[32];
  sprintf(s, "%06d", n);
  return std::string(s);
}

class IndexConsumer;

struct FileInfo
{
  FileInfo(std::string &rname) : realname(rname) {
    interesting = rname.compare(0, srcdir.length(), srcdir) == 0;
    if (interesting) {
      // Remove the trailing `/' as well.
      realname.erase(0, srcdir.length() + 1);
    }
  }
  std::string realname;
  std::vector<std::string> output;
  bool interesting;
};

struct Comparator {
  bool operator()(std::string s1, std::string s2) {
    return s1 <= s2;
  }
};

class IndexConsumer : public ASTConsumer,
                      public RecursiveASTVisitor<IndexConsumer>,
                      public DiagnosticConsumer
{
private:
  CompilerInstance &ci;
  SourceManager &sm;
  std::ostream *out;
  std::map<std::string, FileInfo *> relmap;
  MangleContext *mMangleContext;

  FileInfo *GetFileInfo(const std::string &filename) {
    std::map<std::string, FileInfo *>::iterator it;
    it = relmap.find(filename);
    if (it == relmap.end()) {
      // We haven't seen this file before. We need to make the FileInfo
      // structure information ourselves
      const char *real = realpath(filename.c_str(), NULL);
      std::string realstr(real ? real : filename.c_str());
      it = relmap.find(realstr);
      if (it == relmap.end()) {
        // Still didn't find it. Make the FileInfo structure
        FileInfo *info = new FileInfo(realstr);
        it = relmap.insert(make_pair(realstr, info)).first;
      }
      it = relmap.insert(make_pair(filename, it->second)).first;
    }
    return it->second;
  }

  FileInfo *GetFileInfo(const char *filename) {
    std::string filenamestr(filename);
    return GetFileInfo(filenamestr);
  }

  // Helpers for processing declarations
  // Should we ignore this location?
  bool IsInterestingLocation(SourceLocation loc) {
    // If we don't have a valid location... it's probably not interesting.
    if (loc.isInvalid())
      return false;
    // I'm not sure this is the best, since it's affected by #line and #file
    // et al. On the other hand, if I just do spelling, I get really wrong
    // values for locations in macros, especially when ## is involved.
    // TODO: So yeah, maybe use sm.getFilename(loc) instead.
    std::string filename = sm.getPresumedLoc(loc).getFilename();
    // Invalid locations and built-ins: not interesting at all
    if (filename[0] == '<')
      return false;

    // Get the real filename
    FileInfo *f = GetFileInfo(filename);
    return f->interesting;
  }

  std::string LocationToString(SourceLocation loc) {
    std::string buffer;
    bool isInvalid;
    // Since we're dealing with only expansion locations here, we should be
    // guaranteed to stay within the same file as "out" points to.
    unsigned column = sm.getExpansionColumnNumber(loc, &isInvalid);

    if (!isInvalid) {
      unsigned line = sm.getExpansionLineNumber(loc, &isInvalid);
      if (!isInvalid) {
        buffer = ToString(line);
        buffer += ":";
        buffer += ToString(column - 1);  // Make 0-based.
      }
    }
    return buffer;
  }

public:
  IndexConsumer(CompilerInstance &ci)
   : ci(ci)
   , sm(ci.getSourceManager())
   , mMangleContext(nullptr)
  {
    //ci.getDiagnostics().setClient(this, false);
    //ci.getPreprocessor().addPPCallbacks(new PreprocThunk(this));
  }

  virtual DiagnosticConsumer *clone(DiagnosticsEngine &Diags) const {
    return new IndexConsumer(ci);
  }

  // All we need is to follow the final declaration.
  virtual void HandleTranslationUnit(ASTContext &ctx) {
    mMangleContext = clang::ItaniumMangleContext::create(ctx, ci.getDiagnostics());

    TraverseDecl(ctx.getTranslationUnitDecl());

    // Emit all files now
    std::map<std::string, FileInfo *>::iterator it;
    for (it = relmap.begin(); it != relmap.end(); it++) {
      if (!it->second->interesting)
        continue;

      FileInfo& info = *it->second;

      std::string filename = outdir;
      filename += it->second->realname;

      // Okay, I want to use the standard library for I/O as much as possible,
      // but the C/C++ standard library does not have the feature of "open
      // succeeds only if it doesn't exist."
      FILE* fp = fopen(filename.c_str(), "w");
      if (fp == nullptr) {
        continue;
      }

      // There seems to be a bug where the comparator is called with the last
      // (invalid) iterator. Add a valid element there so we don't crash.
      info.output.push_back(std::string(""));
      stable_sort(info.output.begin(), info.output.end() - 1, Comparator());

      //write(fd, it->second->realname.c_str(), it->second->realname.length());
      //write(fd, "\n", 1);
      for (std::string& line : info.output) {
        fwrite(line.c_str(), line.length(), 1, fp);
      }
      fclose(fp);
    }
  }

  const CXXMethodDecl* FindRootMethod(const CXXMethodDecl* method) {
    CXXMethodDecl::method_iterator iter = method->begin_overridden_methods();
    CXXMethodDecl::method_iterator end = method->end_overridden_methods();
    for (; iter != end; iter++) {
      return FindRootMethod(*iter);
    }
    return method;
  }

  bool VisitFunctionDecl(FunctionDecl *d) {
    if (!IsInterestingLocation(d->getLocation())) {
      return true;
    }

    std::string mangled = GetMangledName(mMangleContext, d);
    if (CXXMethodDecl::classof(d)) {
      const CXXMethodDecl *method = FindRootMethod(dyn_cast<CXXMethodDecl>(d));
      mangled = GetMangledName(mMangleContext, method);
    }

    std::string loc = LocationToString(d->getLocation());

    StringRef filename = sm.getFilename(d->getLocation());
    FileInfo *f = GetFileInfo(filename);
    const char* kind = d->isThisDeclarationADefinition() ? "def" : "decl";

    char s[1024];
    sprintf(s, "%s %s %s %s\n",
            loc.c_str(), kind, EscapeString(d->getNameAsString()).c_str(), mangled.c_str());

    f->output.push_back(std::string(s));

    return true;
  }

  bool VisitCXXConstructExpr(CXXConstructExpr* e) {
    if (!IsInterestingLocation(e->getLocStart())) {
      return true;
    }

    CXXConstructorDecl* ctor = e->getConstructor();
    std::string mangled = GetMangledName(mMangleContext, ctor);

    if (sm.isMacroBodyExpansion(e->getLocation())) {
      return true;
    }
    if (sm.isMacroArgExpansion(e->getLocation())) {
      return true;
    }

    // Probably want to measure from Loc to ParenOrBraceRange.start (non-inclusive).
    // Would need to do something different for list initialization.

    if (e->getParenOrBraceRange().isInvalid()) {
      return true;
    }

    SourceLocation start = sm.getExpansionLoc(e->getLocation());
    SourceLocation endNonInclusive = sm.getExpansionRange(e->getParenOrBraceRange().getBegin()).first;

    unsigned startOffset = sm.getFileOffset(start);
    unsigned endOffset = sm.getFileOffset(endNonInclusive);

    std::string loc = LocationToString(start);

    const char* startChars = sm.getCharacterData(start);
    std::string text(startChars, endOffset - startOffset);

    StringRef filename = sm.getFilename(e->getLocation());
    FileInfo *f = GetFileInfo(filename);

    char s[1024];
    sprintf(s, "%s use %s %s\n",
            loc.c_str(), EscapeString(text).c_str(), mangled.c_str());

    f->output.push_back(std::string(s));

    return true;
  }

  bool VisitCallExpr(CallExpr *e) {
    if (!IsInterestingLocation(e->getLocStart())) {
      return true;
    }

    Decl *callee = e->getCalleeDecl();
    if (!callee ||
        !IsInterestingLocation(callee->getLocation()) ||
        !NamedDecl::classof(callee)) {
      return true;
    }

    const NamedDecl *namedCallee = dyn_cast<NamedDecl>(callee);

    if (namedCallee) {
      std::string mangled = GetMangledName(mMangleContext, namedCallee);
      if (CXXMethodDecl::classof(namedCallee)) {
        const CXXMethodDecl *method = FindRootMethod(dyn_cast<CXXMethodDecl>(namedCallee));
        mangled = GetMangledName(mMangleContext, method);
      }

      if (sm.isMacroBodyExpansion(e->getCallee()->getLocStart())) {
        return true;
      }
      if (sm.isMacroArgExpansion(e->getCallee()->getLocStart())) {
        return true;
      }

      Expr* callee = e->getCallee()->IgnoreParenImpCasts();

      SourceLocation start, end;
      if (CXXOperatorCallExpr::classof(e)) {
        // Just take the first token.
        CXXOperatorCallExpr* op = dyn_cast<CXXOperatorCallExpr>(e);
        start = end = op->getOperatorLoc();
      } else if (MemberExpr::classof(callee)) {
        MemberExpr* member = dyn_cast<MemberExpr>(callee);
        start = end = member->getMemberLoc();
      } else {
        start = sm.getExpansionLoc(callee->getLocStart());
        end = sm.getExpansionRange(callee->getLocEnd()).second;
      }

      unsigned startOffset = sm.getFileOffset(start);
      unsigned endOffset = sm.getFileOffset(end);

      endOffset += Lexer::MeasureTokenLength(end, sm, ci.getLangOpts());

      std::string loc = LocationToString(start);

      const char* startChars = sm.getCharacterData(start);
      std::string text(startChars, endOffset - startOffset);

      StringRef filename = sm.getFilename(start);
      FileInfo *f = GetFileInfo(filename);

      char s[1024];
      sprintf(s, "%s use %s %s\n", loc.c_str(), EscapeString(text).c_str(), mangled.c_str());

      f->output.push_back(std::string(s));
    }

    return true;
  }

};

class IndexAction : public PluginASTAction
{
protected:
  ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef f) {
    return new IndexConsumer(CI);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args)
  {
    if (args.size() != 2) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Error,
        "Need arguments for the source and output directories");
      D.Report(DiagID);
      return false;
    }
    // Load our directories
    char *abs_src = realpath(args[0].c_str(), NULL);
    if (!abs_src) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Error,
        "Source directory '%0' does not exist");
      D.Report(DiagID) << args[0];
      return false;
    }
    srcdir = abs_src;

    char *abs_outdir = realpath(args[1].c_str(), NULL);
    if (!abs_outdir) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Error,
        "Output directory '%0' does not exist");
      D.Report(DiagID) << args[1];
      return false;
    }
    outdir = abs_outdir;
    outdir += "/";

    return true;
  }

  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Help for mozsearch plugin goes here\n";
  }
};

static FrontendPluginRegistry::Add<IndexAction>
X("mozsearch-index", "create the mozsearch index database");