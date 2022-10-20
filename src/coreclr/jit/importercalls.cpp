// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "jitpch.h"

#define Verify(cond, msg)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            verRaiseVerifyExceptionIfNeeded(INDEBUG(msg) DEBUGARG(__FILE__) DEBUGARG(__LINE__));                       \
        }                                                                                                              \
    } while (0)

//------------------------------------------------------------------------
// impImportCall: import a call-inspiring opcode
//
// Arguments:
//    opcode                    - opcode that inspires the call
//    pResolvedToken            - resolved token for the call target
//    pConstrainedResolvedToken - resolved constraint token (or nullptr)
//    newObjThis                - tree for this pointer or uninitialized newobj temp (or nullptr)
//    prefixFlags               - IL prefix flags for the call
//    callInfo                  - EE supplied info for the call
//    rawILOffset               - IL offset of the opcode, used for guarded devirtualization.
//
// Returns:
//    Type of the call's return value.
//    If we're importing an inlinee and have realized the inline must fail, the call return type should be TYP_UNDEF.
//    However we can't assert for this here yet because there are cases we miss. See issue #13272.
//
//
// Notes:
//    opcode can be CEE_CALL, CEE_CALLI, CEE_CALLVIRT, or CEE_NEWOBJ.
//
//    For CEE_NEWOBJ, newobjThis should be the temp grabbed for the allocated
//    uninitialized object.

#ifdef _PREFAST_
#pragma warning(push)
#pragma warning(disable : 21000) // Suppress PREFast warning about overly large function
#endif

var_types Compiler::impImportCall(OPCODE                  opcode,
                                  CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                  CORINFO_RESOLVED_TOKEN* pConstrainedResolvedToken,
                                  GenTree*                newobjThis,
                                  int                     prefixFlags,
                                  CORINFO_CALL_INFO*      callInfo,
                                  IL_OFFSET               rawILOffset)
{
    assert(opcode == CEE_CALL || opcode == CEE_CALLVIRT || opcode == CEE_NEWOBJ || opcode == CEE_CALLI);

    // The current statement DI may not refer to the exact call, but for calls
    // we wish to be able to attach the exact IL instruction to get "return
    // value" support in the debugger, so create one with the exact IL offset.
    DebugInfo di = impCreateDIWithCurrentStackInfo(rawILOffset, true);

    var_types              callRetTyp                     = TYP_COUNT;
    CORINFO_SIG_INFO*      sig                            = nullptr;
    CORINFO_METHOD_HANDLE  methHnd                        = nullptr;
    CORINFO_CLASS_HANDLE   clsHnd                         = nullptr;
    unsigned               clsFlags                       = 0;
    unsigned               mflags                         = 0;
    GenTree*               call                           = nullptr;
    CORINFO_THIS_TRANSFORM constraintCallThisTransform    = CORINFO_NO_THIS_TRANSFORM;
    CORINFO_CONTEXT_HANDLE exactContextHnd                = nullptr;
    bool                   exactContextNeedsRuntimeLookup = false;
    bool                   canTailCall                    = true;
    const char*            szCanTailCallFailReason        = nullptr;
    const int              tailCallFlags                  = (prefixFlags & PREFIX_TAILCALL);
    const bool             isReadonlyCall                 = (prefixFlags & PREFIX_READONLY) != 0;

    methodPointerInfo* ldftnInfo = nullptr;

    // Synchronized methods need to call CORINFO_HELP_MON_EXIT at the end. We could
    // do that before tailcalls, but that is probably not the intended
    // semantic. So just disallow tailcalls from synchronized methods.
    // Also, popping arguments in a varargs function is more work and NYI
    // If we have a security object, we have to keep our frame around for callers
    // to see any imperative security.
    // Reverse P/Invokes need a call to CORINFO_HELP_JIT_REVERSE_PINVOKE_EXIT
    // at the end, so tailcalls should be disabled.
    if (info.compFlags & CORINFO_FLG_SYNCH)
    {
        canTailCall             = false;
        szCanTailCallFailReason = "Caller is synchronized";
    }
    else if (opts.IsReversePInvoke())
    {
        canTailCall             = false;
        szCanTailCallFailReason = "Caller is Reverse P/Invoke";
    }
#if !FEATURE_FIXED_OUT_ARGS
    else if (info.compIsVarArgs)
    {
        canTailCall             = false;
        szCanTailCallFailReason = "Caller is varargs";
    }
#endif // FEATURE_FIXED_OUT_ARGS

    // We only need to cast the return value of pinvoke inlined calls that return small types

    bool checkForSmallType  = false;
    bool bIntrinsicImported = false;

    CORINFO_SIG_INFO calliSig;
    NewCallArg       extraArg;

    /*-------------------------------------------------------------------------
     * First create the call node
     */

    if (opcode == CEE_CALLI)
    {
        if (IsTargetAbi(CORINFO_NATIVEAOT_ABI))
        {
            // See comment in impCheckForPInvokeCall
            BasicBlock* block = compIsForInlining() ? impInlineInfo->iciBlock : compCurBB;
            if (info.compCompHnd->convertPInvokeCalliToCall(pResolvedToken, !impCanPInvokeInlineCallSite(block)))
            {
                eeGetCallInfo(pResolvedToken, nullptr, CORINFO_CALLINFO_ALLOWINSTPARAM, callInfo);
                return impImportCall(CEE_CALL, pResolvedToken, nullptr, nullptr, prefixFlags, callInfo, rawILOffset);
            }
        }

        /* Get the call site sig */
        eeGetSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, &calliSig);

        callRetTyp = JITtype2varType(calliSig.retType);

        call = impImportIndirectCall(&calliSig, di);

        // We don't know the target method, so we have to infer the flags, or
        // assume the worst-case.
        mflags = (calliSig.callConv & CORINFO_CALLCONV_HASTHIS) ? 0 : CORINFO_FLG_STATIC;

#ifdef DEBUG
        if (verbose)
        {
            unsigned structSize = (callRetTyp == TYP_STRUCT) ? eeTryGetClassSize(calliSig.retTypeSigClass) : 0;
            printf("\nIn Compiler::impImportCall: opcode is %s, kind=%d, callRetType is %s, structSize is %u\n",
                   opcodeNames[opcode], callInfo->kind, varTypeName(callRetTyp), structSize);
        }
#endif
        sig = &calliSig;
    }
    else // (opcode != CEE_CALLI)
    {
        NamedIntrinsic ni = NI_Illegal;

        // Passing CORINFO_CALLINFO_ALLOWINSTPARAM indicates that this JIT is prepared to
        // supply the instantiation parameters necessary to make direct calls to underlying
        // shared generic code, rather than calling through instantiating stubs.  If the
        // returned signature has CORINFO_CALLCONV_PARAMTYPE then this indicates that the JIT
        // must indeed pass an instantiation parameter.

        methHnd = callInfo->hMethod;

        sig        = &(callInfo->sig);
        callRetTyp = JITtype2varType(sig->retType);

        mflags = callInfo->methodFlags;

#ifdef DEBUG
        if (verbose)
        {
            unsigned structSize = (callRetTyp == TYP_STRUCT) ? eeTryGetClassSize(sig->retTypeSigClass) : 0;
            printf("\nIn Compiler::impImportCall: opcode is %s, kind=%d, callRetType is %s, structSize is %u\n",
                   opcodeNames[opcode], callInfo->kind, varTypeName(callRetTyp), structSize);
        }
#endif
        if (compIsForInlining())
        {
            /* Does the inlinee use StackCrawlMark */

            if (mflags & CORINFO_FLG_DONT_INLINE_CALLER)
            {
                compInlineResult->NoteFatal(InlineObservation::CALLEE_STACK_CRAWL_MARK);
                return TYP_UNDEF;
            }

            /* For now ignore varargs */
            if ((sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_NATIVEVARARG)
            {
                compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_NATIVE_VARARGS);
                return TYP_UNDEF;
            }

            if ((sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG)
            {
                compInlineResult->NoteFatal(InlineObservation::CALLEE_HAS_MANAGED_VARARGS);
                return TYP_UNDEF;
            }

            if ((mflags & CORINFO_FLG_VIRTUAL) && (sig->sigInst.methInstCount != 0) && (opcode == CEE_CALLVIRT))
            {
                compInlineResult->NoteFatal(InlineObservation::CALLEE_IS_GENERIC_VIRTUAL);
                return TYP_UNDEF;
            }
        }

        clsHnd = pResolvedToken->hClass;

        clsFlags = callInfo->classFlags;

#ifdef DEBUG
        // If this is a call to JitTestLabel.Mark, do "early inlining", and record the test attribute.

        // This recognition should really be done by knowing the methHnd of the relevant Mark method(s).
        // These should be in corelib.h, and available through a JIT/EE interface call.
        const char* modName;
        const char* className;
        const char* methodName;
        if ((className = eeGetClassName(clsHnd)) != nullptr &&
            strcmp(className, "System.Runtime.CompilerServices.JitTestLabel") == 0 &&
            (methodName = eeGetMethodName(methHnd, &modName)) != nullptr && strcmp(methodName, "Mark") == 0)
        {
            return impImportJitTestLabelMark(sig->numArgs);
        }
#endif // DEBUG

        const bool isIntrinsic = (mflags & CORINFO_FLG_INTRINSIC) != 0;

        // <NICE> Factor this into getCallInfo </NICE>
        bool isSpecialIntrinsic = false;

        if (isIntrinsic || !info.compMatchedVM)
        {
            // For mismatched VM (AltJit) we want to check all methods as intrinsic to ensure
            // we get more accurate codegen. This particularly applies to HWIntrinsic usage

            const bool isTailCall = canTailCall && (tailCallFlags != 0);

            call =
                impIntrinsic(newobjThis, clsHnd, methHnd, sig, mflags, pResolvedToken->token, isReadonlyCall,
                             isTailCall, pConstrainedResolvedToken, callInfo->thisTransform, &ni, &isSpecialIntrinsic);

            if (compDonotInline())
            {
                return TYP_UNDEF;
            }

            if (call != nullptr)
            {
#ifdef FEATURE_READYTORUN
                if (call->OperGet() == GT_INTRINSIC)
                {
                    if (opts.IsReadyToRun())
                    {
                        noway_assert(callInfo->kind == CORINFO_CALL);
                        call->AsIntrinsic()->gtEntryPoint = callInfo->codePointerLookup.constLookup;
                    }
                    else
                    {
                        call->AsIntrinsic()->gtEntryPoint.addr       = nullptr;
                        call->AsIntrinsic()->gtEntryPoint.accessType = IAT_VALUE;
                    }
                }
#endif

                bIntrinsicImported = true;
                goto DONE_CALL;
            }
        }

#ifdef FEATURE_SIMD
        call = impSIMDIntrinsic(opcode, newobjThis, clsHnd, methHnd, sig, mflags, pResolvedToken->token);
        if (call != nullptr)
        {
            bIntrinsicImported = true;
            goto DONE_CALL;
        }
#endif // FEATURE_SIMD

        if ((mflags & CORINFO_FLG_VIRTUAL) && (mflags & CORINFO_FLG_EnC) && (opcode == CEE_CALLVIRT))
        {
            NO_WAY("Virtual call to a function added via EnC is not supported");
        }

        if ((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_DEFAULT &&
            (sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_VARARG &&
            (sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_NATIVEVARARG)
        {
            BADCODE("Bad calling convention");
        }

        //-------------------------------------------------------------------------
        //  Construct the call node
        //
        // Work out what sort of call we're making.
        // Dispense with virtual calls implemented via LDVIRTFTN immediately.

        constraintCallThisTransform    = callInfo->thisTransform;
        exactContextHnd                = callInfo->contextHandle;
        exactContextNeedsRuntimeLookup = callInfo->exactContextNeedsRuntimeLookup;

        switch (callInfo->kind)
        {
            case CORINFO_VIRTUALCALL_STUB:
            {
                assert(!(mflags & CORINFO_FLG_STATIC)); // can't call a static method
                assert(!(clsFlags & CORINFO_FLG_VALUECLASS));
                if (callInfo->stubLookup.lookupKind.needsRuntimeLookup)
                {
                    if (callInfo->stubLookup.lookupKind.runtimeLookupKind == CORINFO_LOOKUP_NOT_SUPPORTED)
                    {
                        // Runtime does not support inlining of all shapes of runtime lookups
                        // Inlining has to be aborted in such a case
                        compInlineResult->NoteFatal(InlineObservation::CALLSITE_HAS_COMPLEX_HANDLE);
                        return TYP_UNDEF;
                    }

                    GenTree* stubAddr = impRuntimeLookupToTree(pResolvedToken, &callInfo->stubLookup, methHnd);

                    // stubAddr tree may require a new temp.
                    // If we're inlining, this may trigger the too many locals inline failure.
                    //
                    // If so, we need to bail out.
                    //
                    if (compDonotInline())
                    {
                        return TYP_UNDEF;
                    }

                    // This is the rough code to set up an indirect stub call
                    assert(stubAddr != nullptr);

                    // The stubAddr may be a
                    // complex expression. As it is evaluated after the args,
                    // it may cause registered args to be spilled. Simply spill it.
                    //
                    unsigned const lclNum = lvaGrabTemp(true DEBUGARG("VirtualCall with runtime lookup"));
                    if (compDonotInline())
                    {
                        return TYP_UNDEF;
                    }

                    impAssignTempGen(lclNum, stubAddr, CHECK_SPILL_NONE);
                    stubAddr = gtNewLclvNode(lclNum, TYP_I_IMPL);

                    // Create the actual call node

                    assert((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_VARARG &&
                           (sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_NATIVEVARARG);

                    call = gtNewIndCallNode(stubAddr, callRetTyp);

                    call->gtFlags |= GTF_EXCEPT | (stubAddr->gtFlags & GTF_GLOB_EFFECT);
                    call->gtFlags |= GTF_CALL_VIRT_STUB;

#ifdef TARGET_X86
                    // No tailcalls allowed for these yet...
                    canTailCall             = false;
                    szCanTailCallFailReason = "VirtualCall with runtime lookup";
#endif
                }
                else
                {
                    // The stub address is known at compile time
                    call                               = gtNewCallNode(CT_USER_FUNC, callInfo->hMethod, callRetTyp, di);
                    call->AsCall()->gtStubCallStubAddr = callInfo->stubLookup.constLookup.addr;
                    call->gtFlags |= GTF_CALL_VIRT_STUB;
                    assert(callInfo->stubLookup.constLookup.accessType != IAT_PPVALUE &&
                           callInfo->stubLookup.constLookup.accessType != IAT_RELPVALUE);
                    if (callInfo->stubLookup.constLookup.accessType == IAT_PVALUE)
                    {
                        call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_VIRTSTUB_REL_INDIRECT;
                    }
                }

#ifdef FEATURE_READYTORUN
                if (opts.IsReadyToRun())
                {
                    // Null check is sometimes needed for ready to run to handle
                    // non-virtual <-> virtual changes between versions
                    if (callInfo->nullInstanceCheck)
                    {
                        call->gtFlags |= GTF_CALL_NULLCHECK;
                    }
                }
#endif

                break;
            }

            case CORINFO_VIRTUALCALL_VTABLE:
            {
                assert(!(mflags & CORINFO_FLG_STATIC)); // can't call a static method
                assert(!(clsFlags & CORINFO_FLG_VALUECLASS));
                call = gtNewCallNode(CT_USER_FUNC, callInfo->hMethod, callRetTyp, di);
                call->gtFlags |= GTF_CALL_VIRT_VTABLE;

                // Mark this method to expand the virtual call target early in fgMorphCall
                call->AsCall()->SetExpandedEarly();
                break;
            }

            case CORINFO_VIRTUALCALL_LDVIRTFTN:
            {
                if (compIsForInlining())
                {
                    compInlineResult->NoteFatal(InlineObservation::CALLSITE_HAS_CALL_VIA_LDVIRTFTN);
                    return TYP_UNDEF;
                }

                assert(!(mflags & CORINFO_FLG_STATIC)); // can't call a static method
                assert(!(clsFlags & CORINFO_FLG_VALUECLASS));
                // OK, We've been told to call via LDVIRTFTN, so just
                // take the call now....
                call = gtNewIndCallNode(nullptr, callRetTyp, di);

                impPopCallArgs(sig, call->AsCall());

                GenTree* thisPtr = impPopStack().val;
                thisPtr          = impTransformThis(thisPtr, pConstrainedResolvedToken, callInfo->thisTransform);
                assert(thisPtr != nullptr);

                // Clone the (possibly transformed) "this" pointer
                GenTree* thisPtrCopy;
                thisPtr = impCloneExpr(thisPtr, &thisPtrCopy, NO_CLASS_HANDLE, CHECK_SPILL_ALL,
                                       nullptr DEBUGARG("LDVIRTFTN this pointer"));

                GenTree* fptr = impImportLdvirtftn(thisPtr, pResolvedToken, callInfo);
                assert(fptr != nullptr);

                call->AsCall()
                    ->gtArgs.PushFront(this, NewCallArg::Primitive(thisPtrCopy).WellKnown(WellKnownArg::ThisPointer));

                // Now make an indirect call through the function pointer

                unsigned lclNum = lvaGrabTemp(true DEBUGARG("VirtualCall through function pointer"));
                impAssignTempGen(lclNum, fptr, CHECK_SPILL_ALL);
                fptr = gtNewLclvNode(lclNum, TYP_I_IMPL);

                call->AsCall()->gtCallAddr = fptr;
                call->gtFlags |= GTF_EXCEPT | (fptr->gtFlags & GTF_GLOB_EFFECT);

                if ((sig->sigInst.methInstCount != 0) && IsTargetAbi(CORINFO_NATIVEAOT_ABI))
                {
                    // NativeAOT generic virtual method: need to handle potential fat function pointers
                    addFatPointerCandidate(call->AsCall());
                }
#ifdef FEATURE_READYTORUN
                if (opts.IsReadyToRun())
                {
                    // Null check is needed for ready to run to handle
                    // non-virtual <-> virtual changes between versions
                    call->gtFlags |= GTF_CALL_NULLCHECK;
                }
#endif

                // Sine we are jumping over some code, check that its OK to skip that code
                assert((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_VARARG &&
                       (sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_NATIVEVARARG);
                goto DONE;
            }

            case CORINFO_CALL:
            {
                // This is for a non-virtual, non-interface etc. call
                call = gtNewCallNode(CT_USER_FUNC, callInfo->hMethod, callRetTyp, di);

                // We remove the nullcheck for the GetType call intrinsic.
                // TODO-CQ: JIT64 does not introduce the null check for many more helper calls
                // and intrinsics.
                if (callInfo->nullInstanceCheck &&
                    !((mflags & CORINFO_FLG_INTRINSIC) != 0 && (ni == NI_System_Object_GetType)))
                {
                    call->gtFlags |= GTF_CALL_NULLCHECK;
                }

#ifdef FEATURE_READYTORUN
                if (opts.IsReadyToRun())
                {
                    call->AsCall()->setEntryPoint(callInfo->codePointerLookup.constLookup);
                }
#endif
                break;
            }

            case CORINFO_CALL_CODE_POINTER:
            {
                // The EE has asked us to call by computing a code pointer and then doing an
                // indirect call.  This is because a runtime lookup is required to get the code entry point.

                // These calls always follow a uniform calling convention, i.e. no extra hidden params
                assert((sig->callConv & CORINFO_CALLCONV_PARAMTYPE) == 0);

                assert((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_VARARG);
                assert((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_NATIVEVARARG);

                GenTree* fptr =
                    impLookupToTree(pResolvedToken, &callInfo->codePointerLookup, GTF_ICON_FTN_ADDR, callInfo->hMethod);

                if (compDonotInline())
                {
                    return TYP_UNDEF;
                }

                // Now make an indirect call through the function pointer

                unsigned lclNum = lvaGrabTemp(true DEBUGARG("Indirect call through function pointer"));
                impAssignTempGen(lclNum, fptr, CHECK_SPILL_ALL);
                fptr = gtNewLclvNode(lclNum, TYP_I_IMPL);

                call = gtNewIndCallNode(fptr, callRetTyp, di);
                call->gtFlags |= GTF_EXCEPT | (fptr->gtFlags & GTF_GLOB_EFFECT);
                if (callInfo->nullInstanceCheck)
                {
                    call->gtFlags |= GTF_CALL_NULLCHECK;
                }

                break;
            }

            default:
                assert(!"unknown call kind");
                break;
        }

        //-------------------------------------------------------------------------
        // Set more flags

        PREFIX_ASSUME(call != nullptr);

        if (mflags & CORINFO_FLG_NOGCCHECK)
        {
            call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_NOGCCHECK;
        }

        // Mark call if it's one of the ones we will maybe treat as an intrinsic
        if (isSpecialIntrinsic)
        {
            call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_SPECIAL_INTRINSIC;
        }
    }
    assert(sig);
    assert(clsHnd || (opcode == CEE_CALLI)); // We're never verifying for CALLI, so this is not set.

    /* Some sanity checks */

    // CALL_VIRT and NEWOBJ must have a THIS pointer
    assert((opcode != CEE_CALLVIRT && opcode != CEE_NEWOBJ) || (sig->callConv & CORINFO_CALLCONV_HASTHIS));
    // static bit and hasThis are negations of one another
    assert(((mflags & CORINFO_FLG_STATIC) != 0) == ((sig->callConv & CORINFO_CALLCONV_HASTHIS) == 0));
    assert(call != nullptr);

    /*-------------------------------------------------------------------------
     * Check special-cases etc
     */

    /* Special case - Check if it is a call to Delegate.Invoke(). */

    if (mflags & CORINFO_FLG_DELEGATE_INVOKE)
    {
        assert(!(mflags & CORINFO_FLG_STATIC)); // can't call a static method
        assert(mflags & CORINFO_FLG_FINAL);

        /* Set the delegate flag */
        call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_DELEGATE_INV;

        if (callInfo->wrapperDelegateInvoke)
        {
            call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_WRAPPER_DELEGATE_INV;
        }

        if (opcode == CEE_CALLVIRT)
        {
            assert(mflags & CORINFO_FLG_FINAL);

            /* It should have the GTF_CALL_NULLCHECK flag set. Reset it */
            assert(call->gtFlags & GTF_CALL_NULLCHECK);
            call->gtFlags &= ~GTF_CALL_NULLCHECK;
        }
    }

    CORINFO_CLASS_HANDLE actualMethodRetTypeSigClass;
    actualMethodRetTypeSigClass = sig->retTypeSigClass;

    /* Check for varargs */
    if (!compFeatureVarArg() && ((sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG ||
                                 (sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_NATIVEVARARG))
    {
        BADCODE("Varargs not supported.");
    }

    if ((sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG ||
        (sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_NATIVEVARARG)
    {
        assert(!compIsForInlining());

        /* Set the right flags */

        call->gtFlags |= GTF_CALL_POP_ARGS;
        call->AsCall()->gtArgs.SetIsVarArgs();

        /* Can't allow tailcall for varargs as it is caller-pop. The caller
           will be expecting to pop a certain number of arguments, but if we
           tailcall to a function with a different number of arguments, we
           are hosed. There are ways around this (caller remembers esp value,
           varargs is not caller-pop, etc), but not worth it. */
        CLANG_FORMAT_COMMENT_ANCHOR;

#ifdef TARGET_X86
        if (canTailCall)
        {
            canTailCall             = false;
            szCanTailCallFailReason = "Callee is varargs";
        }
#endif

        /* Get the total number of arguments - this is already correct
         * for CALLI - for methods we have to get it from the call site */

        if (opcode != CEE_CALLI)
        {
#ifdef DEBUG
            unsigned numArgsDef = sig->numArgs;
#endif
            eeGetCallSiteSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, sig);

            // For vararg calls we must be sure to load the return type of the
            // method actually being called, as well as the return types of the
            // specified in the vararg signature. With type equivalency, these types
            // may not be the same.
            if (sig->retTypeSigClass != actualMethodRetTypeSigClass)
            {
                if (actualMethodRetTypeSigClass != nullptr && sig->retType != CORINFO_TYPE_CLASS &&
                    sig->retType != CORINFO_TYPE_BYREF && sig->retType != CORINFO_TYPE_PTR &&
                    sig->retType != CORINFO_TYPE_VAR)
                {
                    // Make sure that all valuetypes (including enums) that we push are loaded.
                    // This is to guarantee that if a GC is triggered from the prestub of this methods,
                    // all valuetypes in the method signature are already loaded.
                    // We need to be able to find the size of the valuetypes, but we cannot
                    // do a class-load from within GC.
                    info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(actualMethodRetTypeSigClass);
                }
            }

            assert(numArgsDef <= sig->numArgs);
        }

        /* We will have "cookie" as the last argument but we cannot push
         * it on the operand stack because we may overflow, so we append it
         * to the arg list next after we pop them */
    }

    //--------------------------- Inline NDirect ------------------------------

    // For inline cases we technically should look at both the current
    // block and the call site block (or just the latter if we've
    // fused the EH trees). However the block-related checks pertain to
    // EH and we currently won't inline a method with EH. So for
    // inlinees, just checking the call site block is sufficient.
    {
        // New lexical block here to avoid compilation errors because of GOTOs.
        BasicBlock* block = compIsForInlining() ? impInlineInfo->iciBlock : compCurBB;
        impCheckForPInvokeCall(call->AsCall(), methHnd, sig, mflags, block);
    }

#ifdef UNIX_X86_ABI
    // On Unix x86 we use caller-cleaned convention.
    if ((call->gtFlags & GTF_CALL_UNMANAGED) == 0)
        call->gtFlags |= GTF_CALL_POP_ARGS;
#endif // UNIX_X86_ABI

    if (call->gtFlags & GTF_CALL_UNMANAGED)
    {
        // We set up the unmanaged call by linking the frame, disabling GC, etc
        // This needs to be cleaned up on return.
        // In addition, native calls have different normalization rules than managed code
        // (managed calling convention always widens return values in the callee)
        if (canTailCall)
        {
            canTailCall             = false;
            szCanTailCallFailReason = "Callee is native";
        }

        checkForSmallType = true;

        impPopArgsForUnmanagedCall(call->AsCall(), sig);

        goto DONE;
    }
    else if ((opcode == CEE_CALLI) && ((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_DEFAULT) &&
             ((sig->callConv & CORINFO_CALLCONV_MASK) != CORINFO_CALLCONV_VARARG))
    {
        if (!info.compCompHnd->canGetCookieForPInvokeCalliSig(sig))
        {
            // Normally this only happens with inlining.
            // However, a generic method (or type) being NGENd into another module
            // can run into this issue as well.  There's not an easy fall-back for NGEN
            // so instead we fallback to JIT.
            if (compIsForInlining())
            {
                compInlineResult->NoteFatal(InlineObservation::CALLSITE_CANT_EMBED_PINVOKE_COOKIE);
            }
            else
            {
                IMPL_LIMITATION("Can't get PInvoke cookie (cross module generics)");
            }

            return TYP_UNDEF;
        }

        GenTree* cookie = eeGetPInvokeCookie(sig);

        // This cookie is required to be either a simple GT_CNS_INT or
        // an indirection of a GT_CNS_INT
        //
        GenTree* cookieConst = cookie;
        if (cookie->gtOper == GT_IND)
        {
            cookieConst = cookie->AsOp()->gtOp1;
        }
        assert(cookieConst->gtOper == GT_CNS_INT);

        // Setting GTF_DONT_CSE on the GT_CNS_INT as well as on the GT_IND (if it exists) will ensure that
        // we won't allow this tree to participate in any CSE logic
        //
        cookie->gtFlags |= GTF_DONT_CSE;
        cookieConst->gtFlags |= GTF_DONT_CSE;

        call->AsCall()->gtCallCookie = cookie;

        if (canTailCall)
        {
            canTailCall             = false;
            szCanTailCallFailReason = "PInvoke calli";
        }
    }

    /*-------------------------------------------------------------------------
     * Create the argument list
     */

    //-------------------------------------------------------------------------
    // Special case - for varargs we have an implicit last argument

    if ((sig->callConv & CORINFO_CALLCONV_MASK) == CORINFO_CALLCONV_VARARG)
    {
        assert(!compIsForInlining());

        void *varCookie, *pVarCookie;
        if (!info.compCompHnd->canGetVarArgsHandle(sig))
        {
            compInlineResult->NoteFatal(InlineObservation::CALLSITE_CANT_EMBED_VARARGS_COOKIE);
            return TYP_UNDEF;
        }

        varCookie = info.compCompHnd->getVarArgsHandle(sig, &pVarCookie);
        assert((!varCookie) != (!pVarCookie));
        GenTree* cookieNode = gtNewIconEmbHndNode(varCookie, pVarCookie, GTF_ICON_VARG_HDL, sig);
        assert(extraArg.Node == nullptr);
        extraArg = NewCallArg::Primitive(cookieNode).WellKnown(WellKnownArg::VarArgsCookie);
    }

    //-------------------------------------------------------------------------
    // Extra arg for shared generic code and array methods
    //
    // Extra argument containing instantiation information is passed in the
    // following circumstances:
    // (a) To the "Address" method on array classes; the extra parameter is
    //     the array's type handle (a TypeDesc)
    // (b) To shared-code instance methods in generic structs; the extra parameter
    //     is the struct's type handle (a vtable ptr)
    // (c) To shared-code per-instantiation non-generic static methods in generic
    //     classes and structs; the extra parameter is the type handle
    // (d) To shared-code generic methods; the extra parameter is an
    //     exact-instantiation MethodDesc
    //
    // We also set the exact type context associated with the call so we can
    // inline the call correctly later on.

    if (sig->callConv & CORINFO_CALLCONV_PARAMTYPE)
    {
        assert(call->AsCall()->gtCallType == CT_USER_FUNC);
        if (clsHnd == nullptr)
        {
            NO_WAY("CALLI on parameterized type");
        }

        assert(opcode != CEE_CALLI);

        GenTree* instParam;
        bool     runtimeLookup;

        // Instantiated generic method
        if (((SIZE_T)exactContextHnd & CORINFO_CONTEXTFLAGS_MASK) == CORINFO_CONTEXTFLAGS_METHOD)
        {
            assert(exactContextHnd != METHOD_BEING_COMPILED_CONTEXT());

            CORINFO_METHOD_HANDLE exactMethodHandle =
                (CORINFO_METHOD_HANDLE)((SIZE_T)exactContextHnd & ~CORINFO_CONTEXTFLAGS_MASK);

            if (!exactContextNeedsRuntimeLookup)
            {
#ifdef FEATURE_READYTORUN
                if (opts.IsReadyToRun())
                {
                    instParam =
                        impReadyToRunLookupToTree(&callInfo->instParamLookup, GTF_ICON_METHOD_HDL, exactMethodHandle);
                    if (instParam == nullptr)
                    {
                        assert(compDonotInline());
                        return TYP_UNDEF;
                    }
                }
                else
#endif
                {
                    instParam = gtNewIconEmbMethHndNode(exactMethodHandle);
                    info.compCompHnd->methodMustBeLoadedBeforeCodeIsRun(exactMethodHandle);
                }
            }
            else
            {
                instParam = impTokenToHandle(pResolvedToken, &runtimeLookup, true /*mustRestoreHandle*/);
                if (instParam == nullptr)
                {
                    assert(compDonotInline());
                    return TYP_UNDEF;
                }
            }
        }

        // otherwise must be an instance method in a generic struct,
        // a static method in a generic type, or a runtime-generated array method
        else
        {
            assert(((SIZE_T)exactContextHnd & CORINFO_CONTEXTFLAGS_MASK) == CORINFO_CONTEXTFLAGS_CLASS);
            CORINFO_CLASS_HANDLE exactClassHandle = eeGetClassFromContext(exactContextHnd);

            if (compIsForInlining() && (clsFlags & CORINFO_FLG_ARRAY) != 0)
            {
                compInlineResult->NoteFatal(InlineObservation::CALLEE_IS_ARRAY_METHOD);
                return TYP_UNDEF;
            }

            if ((clsFlags & CORINFO_FLG_ARRAY) && isReadonlyCall)
            {
                // We indicate "readonly" to the Address operation by using a null
                // instParam.
                instParam = gtNewIconNode(0, TYP_REF);
            }
            else if (!exactContextNeedsRuntimeLookup)
            {
#ifdef FEATURE_READYTORUN
                if (opts.IsReadyToRun())
                {
                    instParam =
                        impReadyToRunLookupToTree(&callInfo->instParamLookup, GTF_ICON_CLASS_HDL, exactClassHandle);
                    if (instParam == nullptr)
                    {
                        assert(compDonotInline());
                        return TYP_UNDEF;
                    }
                }
                else
#endif
                {
                    instParam = gtNewIconEmbClsHndNode(exactClassHandle);
                    info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(exactClassHandle);
                }
            }
            else
            {
                instParam = impParentClassTokenToHandle(pResolvedToken, &runtimeLookup, true /*mustRestoreHandle*/);
                if (instParam == nullptr)
                {
                    assert(compDonotInline());
                    return TYP_UNDEF;
                }
            }
        }

        assert(extraArg.Node == nullptr);
        extraArg = NewCallArg::Primitive(instParam).WellKnown(WellKnownArg::InstParam);
    }

    if ((opcode == CEE_NEWOBJ) && ((clsFlags & CORINFO_FLG_DELEGATE) != 0))
    {
        // Only verifiable cases are supported.
        // dup; ldvirtftn; newobj; or ldftn; newobj.
        // IL test could contain unverifiable sequence, in this case optimization should not be done.
        if (impStackHeight() > 0)
        {
            typeInfo delegateTypeInfo = impStackTop().seTypeInfo;
            if (delegateTypeInfo.IsMethod())
            {
                ldftnInfo = delegateTypeInfo.GetMethodPointerInfo();
            }
        }
    }

    //-------------------------------------------------------------------------
    // The main group of arguments

    impPopCallArgs(sig, call->AsCall());
    if (extraArg.Node != nullptr)
    {
        if (Target::g_tgtArgOrder == Target::ARG_ORDER_R2L)
        {
            call->AsCall()->gtArgs.PushFront(this, extraArg);
        }
        else
        {
            call->AsCall()->gtArgs.PushBack(this, extraArg);
        }

        call->gtFlags |= extraArg.Node->gtFlags & GTF_GLOB_EFFECT;
    }

    //-------------------------------------------------------------------------
    // The "this" pointer

    if (((mflags & CORINFO_FLG_STATIC) == 0) && ((sig->callConv & CORINFO_CALLCONV_EXPLICITTHIS) == 0) &&
        !((opcode == CEE_NEWOBJ) && (newobjThis == nullptr)))
    {
        GenTree* obj;

        if (opcode == CEE_NEWOBJ)
        {
            obj = newobjThis;
        }
        else
        {
            obj = impPopStack().val;
            obj = impTransformThis(obj, pConstrainedResolvedToken, constraintCallThisTransform);
            if (compDonotInline())
            {
                return TYP_UNDEF;
            }
        }

        // Store the "this" value in the call
        call->gtFlags |= obj->gtFlags & GTF_GLOB_EFFECT;
        call->AsCall()->gtArgs.PushFront(this, NewCallArg::Primitive(obj).WellKnown(WellKnownArg::ThisPointer));

        if (impIsThis(obj))
        {
            call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_NONVIRT_SAME_THIS;
        }
    }

    bool probing;
    probing = impConsiderCallProbe(call->AsCall(), rawILOffset);

    // See if we can devirt if we aren't probing.
    if (!probing && opts.OptimizationEnabled())
    {
        if (call->AsCall()->IsVirtual())
        {
            // only true object pointers can be virtual
            assert(call->AsCall()->gtArgs.HasThisPointer() &&
                   call->AsCall()->gtArgs.GetThisArg()->GetNode()->TypeIs(TYP_REF));

            // See if we can devirtualize.

            const bool isExplicitTailCall     = (tailCallFlags & PREFIX_TAILCALL_EXPLICIT) != 0;
            const bool isLateDevirtualization = false;
            impDevirtualizeCall(call->AsCall(), pResolvedToken, &callInfo->hMethod, &callInfo->methodFlags,
                                &callInfo->contextHandle, &exactContextHnd, isLateDevirtualization, isExplicitTailCall,
                                // Take care to pass raw IL offset here as the 'debug info' might be different for
                                // inlinees.
                                rawILOffset);

            // Devirtualization may change which method gets invoked. Update our local cache.
            //
            methHnd = callInfo->hMethod;
        }
        else if (call->AsCall()->IsDelegateInvoke())
        {
            considerGuardedDevirtualization(call->AsCall(), rawILOffset, false, NO_METHOD_HANDLE, NO_CLASS_HANDLE,
                                            nullptr);
        }
    }

    //-------------------------------------------------------------------------
    // The "this" pointer for "newobj"

    if (opcode == CEE_NEWOBJ)
    {
        if (clsFlags & CORINFO_FLG_VAROBJSIZE)
        {
            assert(!(clsFlags & CORINFO_FLG_ARRAY)); // arrays handled separately
            // This is a 'new' of a variable sized object, wher
            // the constructor is to return the object.  In this case
            // the constructor claims to return VOID but we know it
            // actually returns the new object
            assert(callRetTyp == TYP_VOID);
            callRetTyp   = TYP_REF;
            call->gtType = TYP_REF;
            impSpillSpecialSideEff();

            impPushOnStack(call, typeInfo(TI_REF, clsHnd));
        }
        else
        {
            if (clsFlags & CORINFO_FLG_DELEGATE)
            {
                // New inliner morph it in impImportCall.
                // This will allow us to inline the call to the delegate constructor.
                call = fgOptimizeDelegateConstructor(call->AsCall(), &exactContextHnd, ldftnInfo);
            }

            if (!bIntrinsicImported)
            {

#if defined(DEBUG) || defined(INLINE_DATA)

                // Keep track of the raw IL offset of the call
                call->AsCall()->gtRawILOffset = rawILOffset;

#endif // defined(DEBUG) || defined(INLINE_DATA)

                // Is it an inline candidate?
                impMarkInlineCandidate(call, exactContextHnd, exactContextNeedsRuntimeLookup, callInfo, rawILOffset);
            }

            // append the call node.
            impAppendTree(call, CHECK_SPILL_ALL, impCurStmtDI);

            // Now push the value of the 'new onto the stack

            // This is a 'new' of a non-variable sized object.
            // Append the new node (op1) to the statement list,
            // and then push the local holding the value of this
            // new instruction on the stack.

            if (clsFlags & CORINFO_FLG_VALUECLASS)
            {
                assert(newobjThis->gtOper == GT_ADDR && newobjThis->AsOp()->gtOp1->gtOper == GT_LCL_VAR);

                unsigned tmp = newobjThis->AsOp()->gtOp1->AsLclVarCommon()->GetLclNum();
                impPushOnStack(gtNewLclvNode(tmp, lvaGetRealType(tmp)), verMakeTypeInfo(clsHnd).NormaliseForStack());
            }
            else
            {
                if (newobjThis->gtOper == GT_COMMA)
                {
                    // We must have inserted the callout. Get the real newobj.
                    newobjThis = newobjThis->AsOp()->gtOp2;
                }

                assert(newobjThis->gtOper == GT_LCL_VAR);
                impPushOnStack(gtNewLclvNode(newobjThis->AsLclVarCommon()->GetLclNum(), TYP_REF),
                               typeInfo(TI_REF, clsHnd));
            }
        }
        return callRetTyp;
    }

DONE:

#ifdef DEBUG
    // In debug we want to be able to register callsites with the EE.
    assert(call->AsCall()->callSig == nullptr);
    call->AsCall()->callSig  = new (this, CMK_Generic) CORINFO_SIG_INFO;
    *call->AsCall()->callSig = *sig;
#endif

    // Final importer checks for calls flagged as tail calls.
    //
    if (tailCallFlags != 0)
    {
        const bool isExplicitTailCall = (tailCallFlags & PREFIX_TAILCALL_EXPLICIT) != 0;
        const bool isImplicitTailCall = (tailCallFlags & PREFIX_TAILCALL_IMPLICIT) != 0;
        const bool isStressTailCall   = (tailCallFlags & PREFIX_TAILCALL_STRESS) != 0;

        // Exactly one of these should be true.
        assert(isExplicitTailCall != isImplicitTailCall);

        // This check cannot be performed for implicit tail calls for the reason
        // that impIsImplicitTailCallCandidate() is not checking whether return
        // types are compatible before marking a call node with PREFIX_TAILCALL_IMPLICIT.
        // As a result it is possible that in the following case, we find that
        // the type stack is non-empty if Callee() is considered for implicit
        // tail calling.
        //      int Caller(..) { .... void Callee(); ret val; ... }
        //
        // Note that we cannot check return type compatibility before ImpImportCall()
        // as we don't have required info or need to duplicate some of the logic of
        // ImpImportCall().
        //
        // For implicit tail calls, we perform this check after return types are
        // known to be compatible.
        if (isExplicitTailCall && (verCurrentState.esStackDepth != 0))
        {
            BADCODE("Stack should be empty after tailcall");
        }

        // For opportunistic tailcalls we allow implicit widening, i.e. tailcalls from int32 -> int16, since the
        // managed calling convention dictates that the callee widens the value. For explicit tailcalls we don't
        // want to require this detail of the calling convention to bubble up to the tailcall helpers
        bool allowWidening = isImplicitTailCall;
        if (canTailCall &&
            !impTailCallRetTypeCompatible(allowWidening, info.compRetType, info.compMethodInfo->args.retTypeClass,
                                          info.compCallConv, callRetTyp, sig->retTypeClass,
                                          call->AsCall()->GetUnmanagedCallConv()))
        {
            canTailCall             = false;
            szCanTailCallFailReason = "Return types are not tail call compatible";
        }

        // Stack empty check for implicit tail calls.
        if (canTailCall && isImplicitTailCall && (verCurrentState.esStackDepth != 0))
        {
#ifdef TARGET_AMD64
            // JIT64 Compatibility:  Opportunistic tail call stack mismatch throws a VerificationException
            // in JIT64, not an InvalidProgramException.
            Verify(false, "Stack should be empty after tailcall");
#else  // TARGET_64BIT
            BADCODE("Stack should be empty after tailcall");
#endif //! TARGET_64BIT
        }

        // assert(compCurBB is not a catch, finally or filter block);
        // assert(compCurBB is not a try block protected by a finally block);
        assert(!isExplicitTailCall || compCurBB->bbJumpKind == BBJ_RETURN);

        // Ask VM for permission to tailcall
        if (canTailCall)
        {
            // True virtual or indirect calls, shouldn't pass in a callee handle.
            CORINFO_METHOD_HANDLE exactCalleeHnd =
                ((call->AsCall()->gtCallType != CT_USER_FUNC) || call->AsCall()->IsVirtual()) ? nullptr : methHnd;

            if (info.compCompHnd->canTailCall(info.compMethodHnd, methHnd, exactCalleeHnd, isExplicitTailCall))
            {
                if (isExplicitTailCall)
                {
                    // In case of explicit tail calls, mark it so that it is not considered
                    // for in-lining.
                    call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_EXPLICIT_TAILCALL;
                    JITDUMP("\nGTF_CALL_M_EXPLICIT_TAILCALL set for call [%06u]\n", dspTreeID(call));

                    if (isStressTailCall)
                    {
                        call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_STRESS_TAILCALL;
                        JITDUMP("\nGTF_CALL_M_STRESS_TAILCALL set for call [%06u]\n", dspTreeID(call));
                    }
                }
                else
                {
#if FEATURE_TAILCALL_OPT
                    // Must be an implicit tail call.
                    assert(isImplicitTailCall);

                    // It is possible that a call node is both an inline candidate and marked
                    // for opportunistic tail calling.  In-lining happens before morhphing of
                    // trees.  If in-lining of an in-line candidate gets aborted for whatever
                    // reason, it will survive to the morphing stage at which point it will be
                    // transformed into a tail call after performing additional checks.

                    call->AsCall()->gtCallMoreFlags |= GTF_CALL_M_IMPLICIT_TAILCALL;
                    JITDUMP("\nGTF_CALL_M_IMPLICIT_TAILCALL set for call [%06u]\n", dspTreeID(call));

#else //! FEATURE_TAILCALL_OPT
                    NYI("Implicit tail call prefix on a target which doesn't support opportunistic tail calls");

#endif // FEATURE_TAILCALL_OPT
                }

                // This might or might not turn into a tailcall. We do more
                // checks in morph. For explicit tailcalls we need more
                // information in morph in case it turns out to be a
                // helper-based tailcall.
                if (isExplicitTailCall)
                {
                    assert(call->AsCall()->tailCallInfo == nullptr);
                    call->AsCall()->tailCallInfo = new (this, CMK_CorTailCallInfo) TailCallSiteInfo;
                    switch (opcode)
                    {
                        case CEE_CALLI:
                            call->AsCall()->tailCallInfo->SetCalli(sig);
                            break;
                        case CEE_CALLVIRT:
                            call->AsCall()->tailCallInfo->SetCallvirt(sig, pResolvedToken);
                            break;
                        default:
                            call->AsCall()->tailCallInfo->SetCall(sig, pResolvedToken);
                            break;
                    }
                }
            }
            else
            {
                // canTailCall reported its reasons already
                canTailCall = false;
                JITDUMP("\ninfo.compCompHnd->canTailCall returned false for call [%06u]\n", dspTreeID(call));
            }
        }
        else
        {
            // If this assert fires it means that canTailCall was set to false without setting a reason!
            assert(szCanTailCallFailReason != nullptr);
            JITDUMP("\nRejecting %splicit tail call for [%06u], reason: '%s'\n", isExplicitTailCall ? "ex" : "im",
                    dspTreeID(call), szCanTailCallFailReason);
            info.compCompHnd->reportTailCallDecision(info.compMethodHnd, methHnd, isExplicitTailCall, TAILCALL_FAIL,
                                                     szCanTailCallFailReason);
        }
    }

    // Note: we assume that small return types are already normalized by the managed callee
    // or by the pinvoke stub for calls to unmanaged code.

    if (!bIntrinsicImported)
    {
        //
        // Things needed to be checked when bIntrinsicImported is false.
        //

        assert(call->gtOper == GT_CALL);
        assert(callInfo != nullptr);

        if (compIsForInlining() && opcode == CEE_CALLVIRT)
        {
            assert(call->AsCall()->gtArgs.HasThisPointer());
            GenTree* callObj = call->AsCall()->gtArgs.GetThisArg()->GetEarlyNode();

            if ((call->AsCall()->IsVirtual() || (call->gtFlags & GTF_CALL_NULLCHECK)) &&
                impInlineIsGuaranteedThisDerefBeforeAnySideEffects(nullptr, &call->AsCall()->gtArgs, callObj,
                                                                   impInlineInfo->inlArgInfo))
            {
                impInlineInfo->thisDereferencedFirst = true;
            }
        }

#if defined(DEBUG) || defined(INLINE_DATA)

        // Keep track of the raw IL offset of the call
        call->AsCall()->gtRawILOffset = rawILOffset;

#endif // defined(DEBUG) || defined(INLINE_DATA)

        // Is it an inline candidate?
        impMarkInlineCandidate(call, exactContextHnd, exactContextNeedsRuntimeLookup, callInfo, rawILOffset);
    }

    // Extra checks for tail calls and tail recursion.
    //
    // A tail recursive call is a potential loop from the current block to the start of the root method.
    // If we see a tail recursive call, mark the blocks from the call site back to the entry as potentially
    // being in a loop.
    //
    // Note: if we're importing an inlinee we don't mark the right set of blocks, but by then it's too
    // late. Currently this doesn't lead to problems. See GitHub issue 33529.
    //
    // OSR also needs to handle tail calls specially:
    // * block profiling in OSR methods needs to ensure probes happen before tail calls, not after.
    // * the root method entry must be imported if there's a recursive tail call or a potentially
    //   inlineable tail call.
    //
    if ((tailCallFlags != 0) && canTailCall)
    {
        if (gtIsRecursiveCall(methHnd))
        {
            assert(verCurrentState.esStackDepth == 0);
            BasicBlock* loopHead = nullptr;
            if (!compIsForInlining() && opts.IsOSR())
            {
                // For root method OSR we may branch back to the actual method entry,
                // which is not fgFirstBB, and which we will need to import.
                assert(fgEntryBB != nullptr);
                loopHead = fgEntryBB;
            }
            else
            {
                // For normal jitting we may branch back to the firstBB; this
                // should already be imported.
                loopHead = fgFirstBB;
            }

            JITDUMP("\nTail recursive call [%06u] in the method. Mark " FMT_BB " to " FMT_BB
                    " as having a backward branch.\n",
                    dspTreeID(call), loopHead->bbNum, compCurBB->bbNum);
            fgMarkBackwardJump(loopHead, compCurBB);

            compMayConvertTailCallToLoop = true;
        }

        // We only do these OSR checks in the root method because:
        // * If we fail to import the root method entry when importing the root method, we can't go back
        //    and import it during inlining. So instead of checking just for recursive tail calls we also
        //    have to check for anything that might introduce a recursive tail call.
        // * We only instrument root method blocks in OSR methods,
        //
        if (opts.IsOSR() && !compIsForInlining())
        {
            // If a root method tail call candidate block is not a BBJ_RETURN, it should have a unique
            // BBJ_RETURN successor. Mark that successor so we can handle it specially during profile
            // instrumentation.
            //
            if (compCurBB->bbJumpKind != BBJ_RETURN)
            {
                BasicBlock* const successor = compCurBB->GetUniqueSucc();
                assert(successor->bbJumpKind == BBJ_RETURN);
                successor->bbFlags |= BBF_TAILCALL_SUCCESSOR;
                optMethodFlags |= OMF_HAS_TAILCALL_SUCCESSOR;
            }

            // If this call might eventually turn into a loop back to method entry, make sure we
            // import the method entry.
            //
            assert(call->IsCall());
            GenTreeCall* const actualCall           = call->AsCall();
            const bool         mustImportEntryBlock = gtIsRecursiveCall(methHnd) || actualCall->IsInlineCandidate() ||
                                              actualCall->IsGuardedDevirtualizationCandidate();

            // Only schedule importation if we're not currently importing.
            //
            if (mustImportEntryBlock && (compCurBB != fgEntryBB))
            {
                JITDUMP("\nOSR: inlineable or recursive tail call [%06u] in the method, so scheduling " FMT_BB
                        " for importation\n",
                        dspTreeID(call), fgEntryBB->bbNum);
                impImportBlockPending(fgEntryBB);
            }
        }
    }

    if ((sig->flags & CORINFO_SIGFLAG_FAT_CALL) != 0)
    {
        assert(opcode == CEE_CALLI || callInfo->kind == CORINFO_CALL_CODE_POINTER);
        addFatPointerCandidate(call->AsCall());
    }

DONE_CALL:
    // Push or append the result of the call
    if (callRetTyp == TYP_VOID)
    {
        if (opcode == CEE_NEWOBJ)
        {
            // we actually did push something, so don't spill the thing we just pushed.
            assert(verCurrentState.esStackDepth > 0);
            impAppendTree(call, verCurrentState.esStackDepth - 1, impCurStmtDI);
        }
        else
        {
            impAppendTree(call, CHECK_SPILL_ALL, impCurStmtDI);
        }
    }
    else
    {
        impSpillSpecialSideEff();

        if (clsFlags & CORINFO_FLG_ARRAY)
        {
            eeGetCallSiteSig(pResolvedToken->token, pResolvedToken->tokenScope, pResolvedToken->tokenContext, sig);
        }

        typeInfo tiRetVal = verMakeTypeInfo(sig->retType, sig->retTypeClass);
        tiRetVal.NormaliseForStack();

        if (call->IsCall())
        {
            // Sometimes "call" is not a GT_CALL (if we imported an intrinsic that didn't turn into a call)

            GenTreeCall* origCall = call->AsCall();

            const bool isFatPointerCandidate              = origCall->IsFatPointerCandidate();
            const bool isInlineCandidate                  = origCall->IsInlineCandidate();
            const bool isGuardedDevirtualizationCandidate = origCall->IsGuardedDevirtualizationCandidate();

            if (varTypeIsStruct(callRetTyp))
            {
                // Need to treat all "split tree" cases here, not just inline candidates
                call = impFixupCallStructReturn(call->AsCall(), sig->retTypeClass);
            }

            // TODO: consider handling fatcalli cases this way too...?
            if (isInlineCandidate || isGuardedDevirtualizationCandidate)
            {
                // We should not have made any adjustments in impFixupCallStructReturn
                // as we defer those until we know the fate of the call.
                assert(call == origCall);

                assert(opts.OptEnabled(CLFLG_INLINING));
                assert(!isFatPointerCandidate); // We should not try to inline calli.

                // Make the call its own tree (spill the stack if needed).
                // Do not consume the debug info here. This is particularly
                // important if we give up on the inline, in which case the
                // call will typically end up in the statement that contains
                // the GT_RET_EXPR that we leave on the stack.
                impAppendTree(call, CHECK_SPILL_ALL, impCurStmtDI, false);

                // TODO: Still using the widened type.
                GenTree* retExpr = gtNewInlineCandidateReturnExpr(call, genActualType(callRetTyp), compCurBB->bbFlags);

                // Link the retExpr to the call so if necessary we can manipulate it later.
                origCall->gtInlineCandidateInfo->retExpr = retExpr;

                // Propagate retExpr as the placeholder for the call.
                call = retExpr;
            }
            else
            {
                // If the call is virtual, and has a generics context, and is not going to have a class probe,
                // record the context for possible use during late devirt.
                //
                // If we ever want to devirt at Tier0, and/or see issues where OSR methods under PGO lose
                // important devirtualizations, we'll want to allow both a class probe and a captured context.
                //
                if (origCall->IsVirtual() && (origCall->gtCallType != CT_INDIRECT) && (exactContextHnd != nullptr) &&
                    (origCall->gtHandleHistogramProfileCandidateInfo == nullptr))
                {
                    JITDUMP("\nSaving context %p for call [%06u]\n", exactContextHnd, dspTreeID(origCall));
                    origCall->gtCallMoreFlags |= GTF_CALL_M_HAS_LATE_DEVIRT_INFO;
                    LateDevirtualizationInfo* const info = new (this, CMK_Inlining) LateDevirtualizationInfo;
                    info->exactContextHnd                = exactContextHnd;
                    origCall->gtLateDevirtualizationInfo = info;
                }

                if (isFatPointerCandidate)
                {
                    // fatPointer candidates should be in statements of the form call() or var = call().
                    // Such form allows to find statements with fat calls without walking through whole trees
                    // and removes problems with cutting trees.
                    assert(!bIntrinsicImported);
                    assert(IsTargetAbi(CORINFO_NATIVEAOT_ABI));
                    if (call->OperGet() != GT_LCL_VAR) // can be already converted by impFixupCallStructReturn.
                    {
                        unsigned   calliSlot = lvaGrabTemp(true DEBUGARG("calli"));
                        LclVarDsc* varDsc    = lvaGetDesc(calliSlot);

                        impAssignTempGen(calliSlot, call, tiRetVal.GetClassHandle(), CHECK_SPILL_NONE);
                        // impAssignTempGen can change src arg list and return type for call that returns struct.
                        var_types type = genActualType(lvaTable[calliSlot].TypeGet());
                        call           = gtNewLclvNode(calliSlot, type);
                    }
                }

                // For non-candidates we must also spill, since we
                // might have locals live on the eval stack that this
                // call can modify.
                //
                // Suppress this for certain well-known call targets
                // that we know won't modify locals, eg calls that are
                // recognized in gtCanOptimizeTypeEquality. Otherwise
                // we may break key fragile pattern matches later on.
                bool spillStack = true;
                if (call->IsCall())
                {
                    GenTreeCall* callNode = call->AsCall();
                    if ((callNode->gtCallType == CT_HELPER) && (gtIsTypeHandleToRuntimeTypeHelper(callNode) ||
                                                                gtIsTypeHandleToRuntimeTypeHandleHelper(callNode)))
                    {
                        spillStack = false;
                    }
                    else if ((callNode->gtCallMoreFlags & GTF_CALL_M_SPECIAL_INTRINSIC) != 0)
                    {
                        spillStack = false;
                    }
                }

                if (spillStack)
                {
                    impSpillSideEffects(true, CHECK_SPILL_ALL DEBUGARG("non-inline candidate call"));
                }
            }
        }

        if (!bIntrinsicImported)
        {
            //-------------------------------------------------------------------------
            //
            /* If the call is of a small type and the callee is managed, the callee will normalize the result
                before returning.
                However, we need to normalize small type values returned by unmanaged
                functions (pinvoke). The pinvoke stub does the normalization, but we need to do it here
                if we use the shorter inlined pinvoke stub. */

            if (checkForSmallType && varTypeIsIntegral(callRetTyp) && genTypeSize(callRetTyp) < genTypeSize(TYP_INT))
            {
                call = gtNewCastNode(genActualType(callRetTyp), call, false, callRetTyp);
            }
        }

        impPushOnStack(call, tiRetVal);
    }

    // VSD functions get a new call target each time we getCallInfo, so clear the cache.
    // Also, the call info cache for CALLI instructions is largely incomplete, so clear it out.
    // if ( (opcode == CEE_CALLI) || (callInfoCache.fetchCallInfo().kind == CORINFO_VIRTUALCALL_STUB))
    //  callInfoCache.uncacheCallInfo();

    return callRetTyp;
}
#ifdef _PREFAST_
#pragma warning(pop)
#endif

#ifdef DEBUG
//
var_types Compiler::impImportJitTestLabelMark(int numArgs)
{
    TestLabelAndNum tlAndN;
    if (numArgs == 2)
    {
        tlAndN.m_num  = 0;
        StackEntry se = impPopStack();
        assert(se.seTypeInfo.GetType() == TI_INT);
        GenTree* val = se.val;
        assert(val->IsCnsIntOrI());
        tlAndN.m_tl = (TestLabel)val->AsIntConCommon()->IconValue();
    }
    else if (numArgs == 3)
    {
        StackEntry se = impPopStack();
        assert(se.seTypeInfo.GetType() == TI_INT);
        GenTree* val = se.val;
        assert(val->IsCnsIntOrI());
        tlAndN.m_num = val->AsIntConCommon()->IconValue();
        se           = impPopStack();
        assert(se.seTypeInfo.GetType() == TI_INT);
        val = se.val;
        assert(val->IsCnsIntOrI());
        tlAndN.m_tl = (TestLabel)val->AsIntConCommon()->IconValue();
    }
    else
    {
        assert(false);
    }

    StackEntry expSe = impPopStack();
    GenTree*   node  = expSe.val;

    // There are a small number of special cases, where we actually put the annotation on a subnode.
    if (tlAndN.m_tl == TL_LoopHoist && tlAndN.m_num >= 100)
    {
        // A loop hoist annotation with value >= 100 means that the expression should be a static field access,
        // a GT_IND of a static field address, which should be the sum of a (hoistable) helper call and possibly some
        // offset within the static field block whose address is returned by the helper call.
        // The annotation is saying that this address calculation, but not the entire access, should be hoisted.
        assert(node->OperGet() == GT_IND);
        tlAndN.m_num -= 100;
        GetNodeTestData()->Set(node->AsOp()->gtOp1, tlAndN);
        GetNodeTestData()->Remove(node);
    }
    else
    {
        GetNodeTestData()->Set(node, tlAndN);
    }

    impPushOnStack(node, expSe.seTypeInfo);
    return node->TypeGet();
}
#endif // DEBUG

//-----------------------------------------------------------------------------------
//  impFixupCallStructReturn: For a call node that returns a struct do one of the following:
//  - set the flag to indicate struct return via retbuf arg;
//  - adjust the return type to a SIMD type if it is returned in 1 reg;
//  - spill call result into a temp if it is returned into 2 registers or more and not tail call or inline candidate.
//
//  Arguments:
//    call       -  GT_CALL GenTree node
//    retClsHnd  -  Class handle of return type of the call
//
//  Return Value:
//    Returns new GenTree node after fixing struct return of call node
//
GenTree* Compiler::impFixupCallStructReturn(GenTreeCall* call, CORINFO_CLASS_HANDLE retClsHnd)
{
    if (!varTypeIsStruct(call))
    {
        return call;
    }

    call->gtRetClsHnd = retClsHnd;

#if FEATURE_MULTIREG_RET
    call->InitializeStructReturnType(this, retClsHnd, call->GetUnmanagedCallConv());
    const ReturnTypeDesc* retTypeDesc = call->GetReturnTypeDesc();
    const unsigned        retRegCount = retTypeDesc->GetReturnRegCount();
#else  // !FEATURE_MULTIREG_RET
    const unsigned retRegCount = 1;
#endif // !FEATURE_MULTIREG_RET

    structPassingKind howToReturnStruct;
    var_types         returnType = getReturnTypeForStruct(retClsHnd, call->GetUnmanagedCallConv(), &howToReturnStruct);

    if (howToReturnStruct == SPK_ByReference)
    {
        assert(returnType == TYP_UNKNOWN);
        call->gtCallMoreFlags |= GTF_CALL_M_RETBUFFARG;

        if (call->IsUnmanaged())
        {
            // Native ABIs do not allow retbufs to alias anything.
            // This is allowed by the managed ABI and impAssignStructPtr will
            // never introduce copies due to this.
            unsigned tmpNum = lvaGrabTemp(true DEBUGARG("Retbuf for unmanaged call"));
            impAssignTempGen(tmpNum, call, retClsHnd, CHECK_SPILL_ALL);
            return gtNewLclvNode(tmpNum, lvaGetDesc(tmpNum)->TypeGet());
        }

        return call;
    }

    // Recognize SIMD types as we do for LCL_VARs,
    // note it could be not the ABI specific type, for example, on x64 we can set 'TYP_SIMD8`
    // for `System.Numerics.Vector2` here but lower will change it to long as ABI dictates.
    var_types simdReturnType = impNormStructType(call->gtRetClsHnd);
    if (simdReturnType != call->TypeGet())
    {
        assert(varTypeIsSIMD(simdReturnType));
        JITDUMP("changing the type of a call [%06u] from %s to %s\n", dspTreeID(call), varTypeName(call->TypeGet()),
                varTypeName(simdReturnType));
        call->ChangeType(simdReturnType);
    }

    if (retRegCount == 1)
    {
        return call;
    }

#if FEATURE_MULTIREG_RET
    assert(varTypeIsStruct(call)); // It could be a SIMD returned in several regs.
    assert(returnType == TYP_STRUCT);
    assert((howToReturnStruct == SPK_ByValueAsHfa) || (howToReturnStruct == SPK_ByValue));

#ifdef UNIX_AMD64_ABI
    // must be a struct returned in two registers
    assert(retRegCount == 2);
#else  // not UNIX_AMD64_ABI
    assert(retRegCount >= 2);
#endif // not UNIX_AMD64_ABI

    if (!call->CanTailCall() && !call->IsInlineCandidate())
    {
        // Force a call returning multi-reg struct to be always of the IR form
        //   tmp = call
        //
        // No need to assign a multi-reg struct to a local var if:
        //  - It is a tail call or
        //  - The call is marked for in-lining later
        return impAssignMultiRegTypeToVar(call, retClsHnd DEBUGARG(call->GetUnmanagedCallConv()));
    }
    return call;
#endif // FEATURE_MULTIREG_RET
}

GenTreeCall* Compiler::impImportIndirectCall(CORINFO_SIG_INFO* sig, const DebugInfo& di)
{
    var_types callRetTyp = JITtype2varType(sig->retType);

    /* The function pointer is on top of the stack - It may be a
     * complex expression. As it is evaluated after the args,
     * it may cause registered args to be spilled. Simply spill it.
     */

    // Ignore this trivial case.
    if (impStackTop().val->gtOper != GT_LCL_VAR)
    {
        impSpillStackEntry(verCurrentState.esStackDepth - 1,
                           BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impImportIndirectCall"));
    }

    /* Get the function pointer */

    GenTree* fptr = impPopStack().val;

    // The function pointer is typically a sized to match the target pointer size
    // However, stubgen IL optimization can change LDC.I8 to LDC.I4
    // See ILCodeStream::LowerOpcode
    assert(genActualType(fptr->gtType) == TYP_I_IMPL || genActualType(fptr->gtType) == TYP_INT);

#ifdef DEBUG
    // This temporary must never be converted to a double in stress mode,
    // because that can introduce a call to the cast helper after the
    // arguments have already been evaluated.

    if (fptr->OperGet() == GT_LCL_VAR)
    {
        lvaTable[fptr->AsLclVarCommon()->GetLclNum()].lvKeepType = 1;
    }
#endif

    /* Create the call node */

    GenTreeCall* call = gtNewIndCallNode(fptr, callRetTyp, di);

    call->gtFlags |= GTF_EXCEPT | (fptr->gtFlags & GTF_GLOB_EFFECT);
#ifdef UNIX_X86_ABI
    call->gtFlags &= ~GTF_CALL_POP_ARGS;
#endif

    return call;
}

/*****************************************************************************/

void Compiler::impPopArgsForUnmanagedCall(GenTreeCall* call, CORINFO_SIG_INFO* sig)
{
    assert(call->gtFlags & GTF_CALL_UNMANAGED);

    /* Since we push the arguments in reverse order (i.e. right -> left)
     * spill any side effects from the stack
     *
     * OBS: If there is only one side effect we do not need to spill it
     *      thus we have to spill all side-effects except last one
     */

    unsigned lastLevelWithSideEffects = UINT_MAX;

    unsigned argsToReverse = sig->numArgs;

    // For "thiscall", the first argument goes in a register. Since its
    // order does not need to be changed, we do not need to spill it

    if (call->unmgdCallConv == CorInfoCallConvExtension::Thiscall)
    {
        assert(argsToReverse);
        argsToReverse--;
    }

#ifndef TARGET_X86
    // Don't reverse args on ARM or x64 - first four args always placed in regs in order
    argsToReverse = 0;
#endif

    for (unsigned level = verCurrentState.esStackDepth - argsToReverse; level < verCurrentState.esStackDepth; level++)
    {
        if (verCurrentState.esStack[level].val->gtFlags & GTF_ORDER_SIDEEFF)
        {
            assert(lastLevelWithSideEffects == UINT_MAX);

            impSpillStackEntry(level,
                               BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impPopArgsForUnmanagedCall - other side effect"));
        }
        else if (verCurrentState.esStack[level].val->gtFlags & GTF_SIDE_EFFECT)
        {
            if (lastLevelWithSideEffects != UINT_MAX)
            {
                /* We had a previous side effect - must spill it */
                impSpillStackEntry(lastLevelWithSideEffects,
                                   BAD_VAR_NUM DEBUGARG(false) DEBUGARG("impPopArgsForUnmanagedCall - side effect"));

                /* Record the level for the current side effect in case we will spill it */
                lastLevelWithSideEffects = level;
            }
            else
            {
                /* This is the first side effect encountered - record its level */

                lastLevelWithSideEffects = level;
            }
        }
    }

    /* The argument list is now "clean" - no out-of-order side effects
     * Pop the argument list in reverse order */

    impPopReverseCallArgs(sig, call, sig->numArgs - argsToReverse);

    if (call->unmgdCallConv == CorInfoCallConvExtension::Thiscall)
    {
        GenTree* thisPtr = call->gtArgs.GetArgByIndex(0)->GetNode();
        impBashVarAddrsToI(thisPtr);
        assert(thisPtr->TypeGet() == TYP_I_IMPL || thisPtr->TypeGet() == TYP_BYREF);
    }

    for (CallArg& arg : call->gtArgs.Args())
    {
        GenTree* argNode = arg.GetEarlyNode();

        // We should not be passing gc typed args to an unmanaged call.
        if (varTypeIsGC(argNode->TypeGet()))
        {
            // Tolerate byrefs by retyping to native int.
            //
            // This is needed or we'll generate inconsistent GC info
            // for this arg at the call site (gc info says byref,
            // pinvoke sig says native int).
            //
            if (argNode->TypeGet() == TYP_BYREF)
            {
                argNode->ChangeType(TYP_I_IMPL);
            }
            else
            {
                assert(!"*** invalid IL: gc ref passed to unmanaged call");
            }
        }
    }
}

//------------------------------------------------------------------------
// impInitializeArrayIntrinsic: Attempts to replace a call to InitializeArray
//    with a GT_COPYBLK node.
//
// Arguments:
//    sig - The InitializeArray signature.
//
// Return Value:
//    A pointer to the newly created GT_COPYBLK node if the replacement succeeds or
//    nullptr otherwise.
//
// Notes:
//    The function recognizes the following IL pattern:
//      ldc <length> or a list of ldc <lower bound>/<length>
//      newarr or newobj
//      dup
//      ldtoken <field handle>
//      call InitializeArray
//    The lower bounds need not be constant except when the array rank is 1.
//    The function recognizes all kinds of arrays thus enabling a small runtime
//    such as NativeAOT to skip providing an implementation for InitializeArray.

GenTree* Compiler::impInitializeArrayIntrinsic(CORINFO_SIG_INFO* sig)
{
    assert(sig->numArgs == 2);

    GenTree* fieldTokenNode = impStackTop(0).val;
    GenTree* arrayLocalNode = impStackTop(1).val;

    //
    // Verify that the field token is known and valid.  Note that it's also
    // possible for the token to come from reflection, in which case we cannot do
    // the optimization and must therefore revert to calling the helper.  You can
    // see an example of this in bvt\DynIL\initarray2.exe (in Main).
    //

    // Check to see if the ldtoken helper call is what we see here.
    if (fieldTokenNode->gtOper != GT_CALL || (fieldTokenNode->AsCall()->gtCallType != CT_HELPER) ||
        (fieldTokenNode->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_FIELDDESC_TO_STUBRUNTIMEFIELD)))
    {
        return nullptr;
    }

    // Strip helper call away
    fieldTokenNode = fieldTokenNode->AsCall()->gtArgs.GetArgByIndex(0)->GetEarlyNode();

    if (fieldTokenNode->gtOper == GT_IND)
    {
        fieldTokenNode = fieldTokenNode->AsOp()->gtOp1;
    }

    // Check for constant
    if (fieldTokenNode->gtOper != GT_CNS_INT)
    {
        return nullptr;
    }

    CORINFO_FIELD_HANDLE fieldToken = (CORINFO_FIELD_HANDLE)fieldTokenNode->AsIntCon()->gtCompileTimeHandle;
    if (!fieldTokenNode->IsIconHandle(GTF_ICON_FIELD_HDL) || (fieldToken == nullptr))
    {
        return nullptr;
    }

    //
    // We need to get the number of elements in the array and the size of each element.
    // We verify that the newarr statement is exactly what we expect it to be.
    // If it's not then we just return NULL and we don't optimize this call
    //

    // It is possible the we don't have any statements in the block yet.
    if (impLastStmt == nullptr)
    {
        return nullptr;
    }

    //
    // We start by looking at the last statement, making sure it's an assignment, and
    // that the target of the assignment is the array passed to InitializeArray.
    //
    GenTree* arrayAssignment = impLastStmt->GetRootNode();
    if ((arrayAssignment->gtOper != GT_ASG) || (arrayAssignment->AsOp()->gtOp1->gtOper != GT_LCL_VAR) ||
        (arrayLocalNode->gtOper != GT_LCL_VAR) || (arrayAssignment->AsOp()->gtOp1->AsLclVarCommon()->GetLclNum() !=
                                                   arrayLocalNode->AsLclVarCommon()->GetLclNum()))
    {
        return nullptr;
    }

    //
    // Make sure that the object being assigned is a helper call.
    //

    GenTree* newArrayCall = arrayAssignment->AsOp()->gtOp2;
    if ((newArrayCall->gtOper != GT_CALL) || (newArrayCall->AsCall()->gtCallType != CT_HELPER))
    {
        return nullptr;
    }

    //
    // Verify that it is one of the new array helpers.
    //

    bool isMDArray = false;

    if (newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_NEWARR_1_DIRECT) &&
        newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_NEWARR_1_OBJ) &&
        newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_NEWARR_1_VC) &&
        newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_NEWARR_1_ALIGN8)
#ifdef FEATURE_READYTORUN
        && newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_READYTORUN_NEWARR_1)
#endif
            )
    {
        if (newArrayCall->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_NEW_MDARR))
        {
            return nullptr;
        }

        isMDArray = true;
    }

    CORINFO_CLASS_HANDLE arrayClsHnd = (CORINFO_CLASS_HANDLE)newArrayCall->AsCall()->compileTimeHelperArgumentHandle;

    //
    // Make sure we found a compile time handle to the array
    //

    if (!arrayClsHnd)
    {
        return nullptr;
    }

    unsigned rank = 0;
    S_UINT32 numElements;

    if (isMDArray)
    {
        rank = info.compCompHnd->getArrayRank(arrayClsHnd);

        if (rank == 0)
        {
            return nullptr;
        }

        assert(newArrayCall->AsCall()->gtArgs.CountArgs() == 3);
        GenTree* numArgsArg = newArrayCall->AsCall()->gtArgs.GetArgByIndex(1)->GetNode();
        GenTree* argsArg    = newArrayCall->AsCall()->gtArgs.GetArgByIndex(2)->GetNode();

        //
        // The number of arguments should be a constant between 1 and 64. The rank can't be 0
        // so at least one length must be present and the rank can't exceed 32 so there can
        // be at most 64 arguments - 32 lengths and 32 lower bounds.
        //

        if ((!numArgsArg->IsCnsIntOrI()) || (numArgsArg->AsIntCon()->IconValue() < 1) ||
            (numArgsArg->AsIntCon()->IconValue() > 64))
        {
            return nullptr;
        }

        unsigned numArgs = static_cast<unsigned>(numArgsArg->AsIntCon()->IconValue());
        bool     lowerBoundsSpecified;

        if (numArgs == rank * 2)
        {
            lowerBoundsSpecified = true;
        }
        else if (numArgs == rank)
        {
            lowerBoundsSpecified = false;

            //
            // If the rank is 1 and a lower bound isn't specified then the runtime creates
            // a SDArray. Note that even if a lower bound is specified it can be 0 and then
            // we get a SDArray as well, see the for loop below.
            //

            if (rank == 1)
            {
                isMDArray = false;
            }
        }
        else
        {
            return nullptr;
        }

        //
        // The rank is known to be at least 1 so we can start with numElements being 1
        // to avoid the need to special case the first dimension.
        //

        numElements = S_UINT32(1);

        struct Match
        {
            static bool IsArgsFieldInit(GenTree* tree, unsigned index, unsigned lvaNewObjArrayArgs)
            {
                return (tree->OperGet() == GT_ASG) && IsArgsFieldIndir(tree->gtGetOp1(), index, lvaNewObjArrayArgs) &&
                       IsArgsAddr(tree->gtGetOp1()->gtGetOp1()->gtGetOp1(), lvaNewObjArrayArgs);
            }

            static bool IsArgsFieldIndir(GenTree* tree, unsigned index, unsigned lvaNewObjArrayArgs)
            {
                return (tree->OperGet() == GT_IND) && (tree->gtGetOp1()->OperGet() == GT_ADD) &&
                       (tree->gtGetOp1()->gtGetOp2()->IsIntegralConst(sizeof(INT32) * index)) &&
                       IsArgsAddr(tree->gtGetOp1()->gtGetOp1(), lvaNewObjArrayArgs);
            }

            static bool IsArgsAddr(GenTree* tree, unsigned lvaNewObjArrayArgs)
            {
                return (tree->OperGet() == GT_ADDR) && (tree->gtGetOp1()->OperGet() == GT_LCL_VAR) &&
                       (tree->gtGetOp1()->AsLclVar()->GetLclNum() == lvaNewObjArrayArgs);
            }

            static bool IsComma(GenTree* tree)
            {
                return (tree != nullptr) && (tree->OperGet() == GT_COMMA);
            }
        };

        unsigned argIndex = 0;
        GenTree* comma;

        for (comma = argsArg; Match::IsComma(comma); comma = comma->gtGetOp2())
        {
            if (lowerBoundsSpecified)
            {
                //
                // In general lower bounds can be ignored because they're not needed to
                // calculate the total number of elements. But for single dimensional arrays
                // we need to know if the lower bound is 0 because in this case the runtime
                // creates a SDArray and this affects the way the array data offset is calculated.
                //

                if (rank == 1)
                {
                    GenTree* lowerBoundAssign = comma->gtGetOp1();
                    assert(Match::IsArgsFieldInit(lowerBoundAssign, argIndex, lvaNewObjArrayArgs));
                    GenTree* lowerBoundNode = lowerBoundAssign->gtGetOp2();

                    if (lowerBoundNode->IsIntegralConst(0))
                    {
                        isMDArray = false;
                    }
                }

                comma = comma->gtGetOp2();
                argIndex++;
            }

            GenTree* lengthNodeAssign = comma->gtGetOp1();
            assert(Match::IsArgsFieldInit(lengthNodeAssign, argIndex, lvaNewObjArrayArgs));
            GenTree* lengthNode = lengthNodeAssign->gtGetOp2();

            if (!lengthNode->IsCnsIntOrI())
            {
                return nullptr;
            }

            numElements *= S_SIZE_T(lengthNode->AsIntCon()->IconValue());
            argIndex++;
        }

        assert((comma != nullptr) && Match::IsArgsAddr(comma, lvaNewObjArrayArgs));

        if (argIndex != numArgs)
        {
            return nullptr;
        }
    }
    else
    {
        //
        // Make sure there are exactly two arguments:  the array class and
        // the number of elements.
        //

        GenTree* arrayLengthNode;

#ifdef FEATURE_READYTORUN
        if (newArrayCall->AsCall()->gtCallMethHnd == eeFindHelper(CORINFO_HELP_READYTORUN_NEWARR_1))
        {
            // Array length is 1st argument for readytorun helper
            arrayLengthNode = newArrayCall->AsCall()->gtArgs.GetArgByIndex(0)->GetNode();
        }
        else
#endif
        {
            // Array length is 2nd argument for regular helper
            arrayLengthNode = newArrayCall->AsCall()->gtArgs.GetArgByIndex(1)->GetNode();
        }

        //
        // This optimization is only valid for a constant array size.
        //
        if (arrayLengthNode->gtOper != GT_CNS_INT)
        {
            return nullptr;
        }

        numElements = S_SIZE_T(arrayLengthNode->AsIntCon()->gtIconVal);

        if (!info.compCompHnd->isSDArray(arrayClsHnd))
        {
            return nullptr;
        }
    }

    CORINFO_CLASS_HANDLE elemClsHnd;
    var_types            elementType = JITtype2varType(info.compCompHnd->getChildType(arrayClsHnd, &elemClsHnd));

    //
    // Note that genTypeSize will return zero for non primitive types, which is exactly
    // what we want (size will then be 0, and we will catch this in the conditional below).
    // Note that we don't expect this to fail for valid binaries, so we assert in the
    // non-verification case (the verification case should not assert but rather correctly
    // handle bad binaries).  This assert is not guarding any specific invariant, but rather
    // saying that we don't expect this to happen, and if it is hit, we need to investigate
    // why.
    //

    S_UINT32 elemSize(genTypeSize(elementType));
    S_UINT32 size = elemSize * S_UINT32(numElements);

    if (size.IsOverflow())
    {
        return nullptr;
    }

    if ((size.Value() == 0) || (varTypeIsGC(elementType)))
    {
        return nullptr;
    }

    void* initData = info.compCompHnd->getArrayInitializationData(fieldToken, size.Value());
    if (!initData)
    {
        return nullptr;
    }

    //
    // At this point we are ready to commit to implementing the InitializeArray
    // intrinsic using a struct assignment.  Pop the arguments from the stack and
    // return the struct assignment node.
    //

    impPopStack();
    impPopStack();

    const unsigned blkSize = size.Value();
    unsigned       dataOffset;

    if (isMDArray)
    {
        dataOffset = eeGetMDArrayDataOffset(rank);
    }
    else
    {
        dataOffset = eeGetArrayDataOffset();
    }

    GenTree* dstAddr = gtNewOperNode(GT_ADD, TYP_BYREF, arrayLocalNode, gtNewIconNode(dataOffset, TYP_I_IMPL));
    GenTree* dst     = new (this, GT_BLK) GenTreeBlk(GT_BLK, TYP_STRUCT, dstAddr, typGetBlkLayout(blkSize));
    GenTree* src     = gtNewIndOfIconHandleNode(TYP_STRUCT, (size_t)initData, GTF_ICON_CONST_PTR, true);

#ifdef DEBUG
    src->gtGetOp1()->AsIntCon()->gtTargetHandle = THT_InitializeArrayIntrinsics;
#endif

    return gtNewBlkOpNode(dst,   // dst
                          src,   // src
                          false, // volatile
                          true); // copyBlock
}

GenTree* Compiler::impCreateSpanIntrinsic(CORINFO_SIG_INFO* sig)
{
    assert(sig->numArgs == 1);
    assert(sig->sigInst.methInstCount == 1);

    GenTree* fieldTokenNode = impStackTop(0).val;

    //
    // Verify that the field token is known and valid.  Note that it's also
    // possible for the token to come from reflection, in which case we cannot do
    // the optimization and must therefore revert to calling the helper.  You can
    // see an example of this in bvt\DynIL\initarray2.exe (in Main).
    //

    // Check to see if the ldtoken helper call is what we see here.
    if (fieldTokenNode->gtOper != GT_CALL || (fieldTokenNode->AsCall()->gtCallType != CT_HELPER) ||
        (fieldTokenNode->AsCall()->gtCallMethHnd != eeFindHelper(CORINFO_HELP_FIELDDESC_TO_STUBRUNTIMEFIELD)))
    {
        return nullptr;
    }

    // Strip helper call away
    fieldTokenNode = fieldTokenNode->AsCall()->gtArgs.GetArgByIndex(0)->GetNode();
    if (fieldTokenNode->gtOper == GT_IND)
    {
        fieldTokenNode = fieldTokenNode->AsOp()->gtOp1;
    }

    // Check for constant
    if (fieldTokenNode->gtOper != GT_CNS_INT)
    {
        return nullptr;
    }

    CORINFO_FIELD_HANDLE fieldToken = (CORINFO_FIELD_HANDLE)fieldTokenNode->AsIntCon()->gtCompileTimeHandle;
    if (!fieldTokenNode->IsIconHandle(GTF_ICON_FIELD_HDL) || (fieldToken == nullptr))
    {
        return nullptr;
    }

    CORINFO_CLASS_HANDLE fieldOwnerHnd = info.compCompHnd->getFieldClass(fieldToken);

    CORINFO_CLASS_HANDLE fieldClsHnd;
    var_types            fieldElementType =
        JITtype2varType(info.compCompHnd->getFieldType(fieldToken, &fieldClsHnd, fieldOwnerHnd));
    unsigned totalFieldSize;

    // Most static initialization data fields are of some structure, but it is possible for them to be of various
    // primitive types as well
    if (fieldElementType == var_types::TYP_STRUCT)
    {
        totalFieldSize = info.compCompHnd->getClassSize(fieldClsHnd);
    }
    else
    {
        totalFieldSize = genTypeSize(fieldElementType);
    }

    // Limit to primitive or enum type - see ArrayNative::GetSpanDataFrom()
    CORINFO_CLASS_HANDLE targetElemHnd = sig->sigInst.methInst[0];
    if (info.compCompHnd->getTypeForPrimitiveValueClass(targetElemHnd) == CORINFO_TYPE_UNDEF)
    {
        return nullptr;
    }

    const unsigned targetElemSize = info.compCompHnd->getClassSize(targetElemHnd);
    assert(targetElemSize != 0);

    const unsigned count = totalFieldSize / targetElemSize;
    if (count == 0)
    {
        return nullptr;
    }

    void* data = info.compCompHnd->getArrayInitializationData(fieldToken, totalFieldSize);
    if (!data)
    {
        return nullptr;
    }

    //
    // Ready to commit to the work
    //

    impPopStack();

    // Turn count and pointer value into constants.
    GenTree* lengthValue  = gtNewIconNode(count, TYP_INT);
    GenTree* pointerValue = gtNewIconHandleNode((size_t)data, GTF_ICON_CONST_PTR);

    // Construct ReadOnlySpan<T> to return.
    CORINFO_CLASS_HANDLE spanHnd     = sig->retTypeClass;
    unsigned             spanTempNum = lvaGrabTemp(true DEBUGARG("ReadOnlySpan<T> for CreateSpan<T>"));
    lvaSetStruct(spanTempNum, spanHnd, false);

    GenTreeLclFld* pointerField    = gtNewLclFldNode(spanTempNum, TYP_BYREF, 0);
    GenTree*       pointerFieldAsg = gtNewAssignNode(pointerField, pointerValue);

    GenTreeLclFld* lengthField    = gtNewLclFldNode(spanTempNum, TYP_INT, TARGET_POINTER_SIZE);
    GenTree*       lengthFieldAsg = gtNewAssignNode(lengthField, lengthValue);

    // Now append a few statements the initialize the span
    impAppendTree(lengthFieldAsg, CHECK_SPILL_NONE, impCurStmtDI);
    impAppendTree(pointerFieldAsg, CHECK_SPILL_NONE, impCurStmtDI);

    // And finally create a tree that points at the span.
    return impCreateLocalNode(spanTempNum DEBUGARG(0));
}

//------------------------------------------------------------------------
// impIntrinsic: possibly expand intrinsic call into alternate IR sequence
//
// Arguments:
//    newobjThis - for constructor calls, the tree for the newly allocated object
//    clsHnd - handle for the intrinsic method's class
//    method - handle for the intrinsic method
//    sig    - signature of the intrinsic method
//    methodFlags - CORINFO_FLG_XXX flags of the intrinsic method
//    memberRef - the token for the intrinsic method
//    readonlyCall - true if call has a readonly prefix
//    tailCall - true if call is in tail position
//    pConstrainedResolvedToken -- resolved token for constrained call, or nullptr
//       if call is not constrained
//    constraintCallThisTransform -- this transform to apply for a constrained call
//    pIntrinsicName [OUT] -- intrinsic name (see enumeration in namedintrinsiclist.h)
//       for "traditional" jit intrinsics
//    isSpecialIntrinsic [OUT] -- set true if intrinsic expansion is a call
//       that is amenable to special downstream optimization opportunities
//
// Returns:
//    IR tree to use in place of the call, or nullptr if the jit should treat
//    the intrinsic call like a normal call.
//
//    pIntrinsicName set to non-illegal value if the call is recognized as a
//    traditional jit intrinsic, even if the intrinsic is not expaned.
//
//    isSpecial set true if the expansion is subject to special
//    optimizations later in the jit processing
//
// Notes:
//    On success the IR tree may be a call to a different method or an inline
//    sequence. If it is a call, then the intrinsic processing here is responsible
//    for handling all the special cases, as upon return to impImportCall
//    expanded intrinsics bypass most of the normal call processing.
//
//    Intrinsics are generally not recognized in minopts and debug codegen.
//
//    However, certain traditional intrinsics are identifed as "must expand"
//    if there is no fallback implementation to invoke; these must be handled
//    in all codegen modes.
//
//    New style intrinsics (where the fallback implementation is in IL) are
//    identified as "must expand" if they are invoked from within their
//    own method bodies.
//
GenTree* Compiler::impIntrinsic(GenTree*                newobjThis,
                                CORINFO_CLASS_HANDLE    clsHnd,
                                CORINFO_METHOD_HANDLE   method,
                                CORINFO_SIG_INFO*       sig,
                                unsigned                methodFlags,
                                int                     memberRef,
                                bool                    readonlyCall,
                                bool                    tailCall,
                                CORINFO_RESOLVED_TOKEN* pConstrainedResolvedToken,
                                CORINFO_THIS_TRANSFORM  constraintCallThisTransform,
                                NamedIntrinsic*         pIntrinsicName,
                                bool*                   isSpecialIntrinsic)
{
    bool       mustExpand  = false;
    bool       isSpecial   = false;
    const bool isIntrinsic = (methodFlags & CORINFO_FLG_INTRINSIC) != 0;

    NamedIntrinsic ni = lookupNamedIntrinsic(method);

    if (isIntrinsic)
    {
        // The recursive non-virtual calls to Jit intrinsics are must-expand by convention.
        mustExpand = gtIsRecursiveCall(method) && !(methodFlags & CORINFO_FLG_VIRTUAL);
    }
    else
    {
        // For mismatched VM (AltJit) we want to check all methods as intrinsic to ensure
        // we get more accurate codegen. This particularly applies to HWIntrinsic usage
        assert(!info.compMatchedVM);
    }

    // We specially support the following on all platforms to allow for dead
    // code optimization and to more generally support recursive intrinsics.

    if (isIntrinsic)
    {
        if (ni == NI_IsSupported_True)
        {
            assert(sig->numArgs == 0);
            return gtNewIconNode(true);
        }

        if (ni == NI_IsSupported_False)
        {
            assert(sig->numArgs == 0);
            return gtNewIconNode(false);
        }

        if (ni == NI_Throw_PlatformNotSupportedException)
        {
            return impUnsupportedNamedIntrinsic(CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED, method, sig, mustExpand);
        }

        if ((ni > NI_SRCS_UNSAFE_START) && (ni < NI_SRCS_UNSAFE_END))
        {
            assert(!mustExpand);
            return impSRCSUnsafeIntrinsic(ni, clsHnd, method, sig);
        }
    }

#ifdef FEATURE_HW_INTRINSICS
    if ((ni > NI_HW_INTRINSIC_START) && (ni < NI_HW_INTRINSIC_END))
    {
        if (!isIntrinsic)
        {
#if defined(TARGET_XARCH)
            // We can't guarantee that all overloads for the xplat intrinsics can be
            // handled by the AltJit, so limit only the platform specific intrinsics
            assert((NI_Vector256_Xor + 1) == NI_X86Base_BitScanForward);

            if (ni < NI_Vector256_Xor)
#elif defined(TARGET_ARM64)
            // We can't guarantee that all overloads for the xplat intrinsics can be
            // handled by the AltJit, so limit only the platform specific intrinsics
            assert((NI_Vector128_Xor + 1) == NI_AdvSimd_Abs);

            if (ni < NI_Vector128_Xor)
#else
#error Unsupported platform
#endif
            {
                // Several of the NI_Vector64/128/256 APIs do not have
                // all overloads as intrinsic today so they will assert
                return nullptr;
            }
        }

        GenTree* hwintrinsic = impHWIntrinsic(ni, clsHnd, method, sig, mustExpand);

        if (mustExpand && (hwintrinsic == nullptr))
        {
            return impUnsupportedNamedIntrinsic(CORINFO_HELP_THROW_NOT_IMPLEMENTED, method, sig, mustExpand);
        }

        return hwintrinsic;
    }

    if (isIntrinsic && (ni > NI_SIMD_AS_HWINTRINSIC_START) && (ni < NI_SIMD_AS_HWINTRINSIC_END))
    {
        // These intrinsics aren't defined recursively and so they will never be mustExpand
        // Instead, they provide software fallbacks that will be executed instead.

        assert(!mustExpand);
        return impSimdAsHWIntrinsic(ni, clsHnd, method, sig, newobjThis);
    }
#endif // FEATURE_HW_INTRINSICS

    if (!isIntrinsic)
    {
        // Outside the cases above, there are many intrinsics which apply to only a
        // subset of overload and where simply matching by name may cause downstream
        // asserts or other failures. Math.Min is one example, where it only applies
        // to the floating-point overloads.
        return nullptr;
    }

    *pIntrinsicName = ni;

    if (ni == NI_System_StubHelpers_GetStubContext)
    {
        // must be done regardless of DbgCode and MinOpts
        return gtNewLclvNode(lvaStubArgumentVar, TYP_I_IMPL);
    }

    if (ni == NI_System_StubHelpers_NextCallReturnAddress)
    {
        // For now we just avoid inlining anything into these methods since
        // this intrinsic is only rarely used. We could do this better if we
        // wanted to by trying to match which call is the one we need to get
        // the return address of.
        info.compHasNextCallRetAddr = true;
        return new (this, GT_LABEL) GenTree(GT_LABEL, TYP_I_IMPL);
    }

    switch (ni)
    {
        // CreateSpan must be expanded for NativeAOT
        case NI_System_Runtime_CompilerServices_RuntimeHelpers_CreateSpan:
        case NI_System_Runtime_CompilerServices_RuntimeHelpers_InitializeArray:
            mustExpand |= IsTargetAbi(CORINFO_NATIVEAOT_ABI);
            break;

        case NI_Internal_Runtime_MethodTable_Of:
        case NI_System_Activator_AllocatorOf:
        case NI_System_Activator_DefaultConstructorOf:
        case NI_System_EETypePtr_EETypePtrOf:
            mustExpand = true;
            break;

        default:
            break;
    }

    GenTree* retNode = nullptr;

    // Under debug and minopts, only expand what is required.
    // NextCallReturnAddress intrinsic returns the return address of the next call.
    // If that call is an intrinsic and is expanded, codegen for NextCallReturnAddress will fail.
    // To avoid that we conservatively expand only required intrinsics in methods that call
    // the NextCallReturnAddress intrinsic.
    if (!mustExpand && (opts.OptimizationDisabled() || info.compHasNextCallRetAddr))
    {
        *pIntrinsicName = NI_Illegal;
        return retNode;
    }

    CorInfoType callJitType = sig->retType;
    var_types   callType    = JITtype2varType(callJitType);

    /* First do the intrinsics which are always smaller than a call */

    if (ni != NI_Illegal)
    {
        assert(retNode == nullptr);
        switch (ni)
        {
            case NI_Array_Address:
            case NI_Array_Get:
            case NI_Array_Set:
                retNode = impArrayAccessIntrinsic(clsHnd, sig, memberRef, readonlyCall, ni);
                break;

            case NI_System_String_Equals:
            {
                retNode = impStringEqualsOrStartsWith(/*startsWith:*/ false, sig, methodFlags);
                break;
            }

            case NI_System_MemoryExtensions_Equals:
            case NI_System_MemoryExtensions_SequenceEqual:
            {
                retNode = impSpanEqualsOrStartsWith(/*startsWith:*/ false, sig, methodFlags);
                break;
            }

            case NI_System_String_StartsWith:
            {
                retNode = impStringEqualsOrStartsWith(/*startsWith:*/ true, sig, methodFlags);
                break;
            }

            case NI_System_MemoryExtensions_StartsWith:
            {
                retNode = impSpanEqualsOrStartsWith(/*startsWith:*/ true, sig, methodFlags);
                break;
            }

            case NI_System_MemoryExtensions_AsSpan:
            case NI_System_String_op_Implicit:
            {
                assert(sig->numArgs == 1);
                isSpecial = impStackTop().val->OperIs(GT_CNS_STR);
                break;
            }

            case NI_System_String_get_Chars:
            {
                GenTree* op2  = impPopStack().val;
                GenTree* op1  = impPopStack().val;
                GenTree* addr = gtNewIndexAddr(op1, op2, TYP_USHORT, NO_CLASS_HANDLE, OFFSETOF__CORINFO_String__chars,
                                               OFFSETOF__CORINFO_String__stringLen);
                retNode = gtNewIndexIndir(addr->AsIndexAddr());
                break;
            }

            case NI_System_String_get_Length:
            {
                GenTree* op1 = impPopStack().val;
                if (op1->OperIs(GT_CNS_STR))
                {
                    // Optimize `ldstr + String::get_Length()` to CNS_INT
                    // e.g. "Hello".Length => 5
                    GenTreeIntCon* iconNode = gtNewStringLiteralLength(op1->AsStrCon());
                    if (iconNode != nullptr)
                    {
                        retNode = iconNode;
                        break;
                    }
                }
                GenTreeArrLen* arrLen = gtNewArrLen(TYP_INT, op1, OFFSETOF__CORINFO_String__stringLen, compCurBB);
                op1                   = arrLen;

                // Getting the length of a null string should throw
                op1->gtFlags |= GTF_EXCEPT;

                retNode = op1;
                break;
            }

            case NI_System_Runtime_CompilerServices_RuntimeHelpers_CreateSpan:
            {
                retNode = impCreateSpanIntrinsic(sig);
                break;
            }

            case NI_System_Runtime_CompilerServices_RuntimeHelpers_InitializeArray:
            {
                retNode = impInitializeArrayIntrinsic(sig);
                break;
            }

            case NI_System_Runtime_CompilerServices_RuntimeHelpers_IsKnownConstant:
            {
                GenTree* op1 = impPopStack().val;
                if (op1->OperIsConst())
                {
                    // op1 is a known constant, replace with 'true'.
                    retNode = gtNewIconNode(1);
                    JITDUMP("\nExpanding RuntimeHelpers.IsKnownConstant to true early\n");
                    // We can also consider FTN_ADDR and typeof(T) here
                }
                else
                {
                    // op1 is not a known constant, we'll do the expansion in morph
                    retNode = new (this, GT_INTRINSIC) GenTreeIntrinsic(TYP_INT, op1, ni, method);
                    JITDUMP("\nConverting RuntimeHelpers.IsKnownConstant to:\n");
                    DISPTREE(retNode);
                }
                break;
            }

            case NI_Internal_Runtime_MethodTable_Of:
            case NI_System_Activator_AllocatorOf:
            case NI_System_Activator_DefaultConstructorOf:
            case NI_System_EETypePtr_EETypePtrOf:
            {
                assert(IsTargetAbi(CORINFO_NATIVEAOT_ABI)); // Only NativeAOT supports it.
                CORINFO_RESOLVED_TOKEN resolvedToken;
                resolvedToken.tokenContext = impTokenLookupContextHandle;
                resolvedToken.tokenScope   = info.compScopeHnd;
                resolvedToken.token        = memberRef;
                resolvedToken.tokenType    = CORINFO_TOKENKIND_Method;

                CORINFO_GENERICHANDLE_RESULT embedInfo;
                info.compCompHnd->expandRawHandleIntrinsic(&resolvedToken, &embedInfo);

                GenTree* rawHandle = impLookupToTree(&resolvedToken, &embedInfo.lookup, gtTokenToIconFlags(memberRef),
                                                     embedInfo.compileTimeHandle);
                if (rawHandle == nullptr)
                {
                    return nullptr;
                }

                noway_assert(genTypeSize(rawHandle->TypeGet()) == genTypeSize(TYP_I_IMPL));

                unsigned rawHandleSlot = lvaGrabTemp(true DEBUGARG("rawHandle"));
                impAssignTempGen(rawHandleSlot, rawHandle, clsHnd, CHECK_SPILL_NONE);

                GenTree*  lclVar     = gtNewLclvNode(rawHandleSlot, TYP_I_IMPL);
                GenTree*  lclVarAddr = gtNewOperNode(GT_ADDR, TYP_I_IMPL, lclVar);
                var_types resultType = JITtype2varType(sig->retType);
                if (resultType == TYP_STRUCT)
                {
                    retNode = gtNewObjNode(sig->retTypeClass, lclVarAddr);
                }
                else
                {
                    retNode = gtNewIndir(resultType, lclVarAddr);
                }
                break;
            }

            case NI_System_Span_get_Item:
            case NI_System_ReadOnlySpan_get_Item:
            {
                // Have index, stack pointer-to Span<T> s on the stack. Expand to:
                //
                // For Span<T>
                //   Comma
                //     BoundsCheck(index, s->_length)
                //     s->_reference + index * sizeof(T)
                //
                // For ReadOnlySpan<T> -- same expansion, as it now returns a readonly ref
                //
                // Signature should show one class type parameter, which
                // we need to examine.
                assert(sig->sigInst.classInstCount == 1);
                assert(sig->numArgs == 1);
                CORINFO_CLASS_HANDLE spanElemHnd = sig->sigInst.classInst[0];
                const unsigned       elemSize    = info.compCompHnd->getClassSize(spanElemHnd);
                assert(elemSize > 0);

                const bool isReadOnly = (ni == NI_System_ReadOnlySpan_get_Item);

                JITDUMP("\nimpIntrinsic: Expanding %sSpan<T>.get_Item, T=%s, sizeof(T)=%u\n",
                        isReadOnly ? "ReadOnly" : "", eeGetClassName(spanElemHnd), elemSize);

                GenTree* index          = impPopStack().val;
                GenTree* ptrToSpan      = impPopStack().val;
                GenTree* indexClone     = nullptr;
                GenTree* ptrToSpanClone = nullptr;
                assert(genActualType(index) == TYP_INT);
                assert(ptrToSpan->TypeGet() == TYP_BYREF);

#if defined(DEBUG)
                if (verbose)
                {
                    printf("with ptr-to-span\n");
                    gtDispTree(ptrToSpan);
                    printf("and index\n");
                    gtDispTree(index);
                }
#endif // defined(DEBUG)

                // We need to use both index and ptr-to-span twice, so clone or spill.
                index = impCloneExpr(index, &indexClone, NO_CLASS_HANDLE, CHECK_SPILL_ALL,
                                     nullptr DEBUGARG("Span.get_Item index"));
                ptrToSpan = impCloneExpr(ptrToSpan, &ptrToSpanClone, NO_CLASS_HANDLE, CHECK_SPILL_ALL,
                                         nullptr DEBUGARG("Span.get_Item ptrToSpan"));

                // Bounds check
                CORINFO_FIELD_HANDLE lengthHnd    = info.compCompHnd->getFieldInClass(clsHnd, 1);
                const unsigned       lengthOffset = info.compCompHnd->getFieldOffset(lengthHnd);
                GenTree*             length       = gtNewFieldRef(TYP_INT, lengthHnd, ptrToSpan, lengthOffset);
                GenTree* boundsCheck = new (this, GT_BOUNDS_CHECK) GenTreeBoundsChk(index, length, SCK_RNGCHK_FAIL);

                // Element access
                index = indexClone;

#ifdef TARGET_64BIT
                if (index->OperGet() == GT_CNS_INT)
                {
                    index->gtType = TYP_I_IMPL;
                }
                else
                {
                    index = gtNewCastNode(TYP_I_IMPL, index, true, TYP_I_IMPL);
                }
#endif

                if (elemSize != 1)
                {
                    GenTree* sizeofNode = gtNewIconNode(static_cast<ssize_t>(elemSize), TYP_I_IMPL);
                    index               = gtNewOperNode(GT_MUL, TYP_I_IMPL, index, sizeofNode);
                }

                CORINFO_FIELD_HANDLE ptrHnd    = info.compCompHnd->getFieldInClass(clsHnd, 0);
                const unsigned       ptrOffset = info.compCompHnd->getFieldOffset(ptrHnd);
                GenTree*             data      = gtNewFieldRef(TYP_BYREF, ptrHnd, ptrToSpanClone, ptrOffset);
                GenTree*             result    = gtNewOperNode(GT_ADD, TYP_BYREF, data, index);

                // Prepare result
                var_types resultType = JITtype2varType(sig->retType);
                assert(resultType == result->TypeGet());
                retNode = gtNewOperNode(GT_COMMA, resultType, boundsCheck, result);

                break;
            }

            case NI_System_RuntimeTypeHandle_GetValueInternal:
            {
                GenTree* op1 = impStackTop(0).val;
                if (op1->gtOper == GT_CALL && (op1->AsCall()->gtCallType == CT_HELPER) &&
                    gtIsTypeHandleToRuntimeTypeHandleHelper(op1->AsCall()))
                {
                    // Old tree
                    // Helper-RuntimeTypeHandle -> TreeToGetNativeTypeHandle
                    //
                    // New tree
                    // TreeToGetNativeTypeHandle

                    // Remove call to helper and return the native TypeHandle pointer that was the parameter
                    // to that helper.

                    op1 = impPopStack().val;

                    // Get native TypeHandle argument to old helper
                    assert(op1->AsCall()->gtArgs.CountArgs() == 1);
                    op1     = op1->AsCall()->gtArgs.GetArgByIndex(0)->GetNode();
                    retNode = op1;
                }
                // Call the regular function.
                break;
            }

            case NI_System_Type_GetTypeFromHandle:
            {
                GenTree*        op1 = impStackTop(0).val;
                CorInfoHelpFunc typeHandleHelper;
                if (op1->gtOper == GT_CALL && (op1->AsCall()->gtCallType == CT_HELPER) &&
                    gtIsTypeHandleToRuntimeTypeHandleHelper(op1->AsCall(), &typeHandleHelper))
                {
                    op1 = impPopStack().val;
                    // Replace helper with a more specialized helper that returns RuntimeType
                    if (typeHandleHelper == CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE)
                    {
                        typeHandleHelper = CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE;
                    }
                    else
                    {
                        assert(typeHandleHelper == CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE_MAYBENULL);
                        typeHandleHelper = CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE_MAYBENULL;
                    }
                    assert(op1->AsCall()->gtArgs.CountArgs() == 1);
                    op1 = gtNewHelperCallNode(typeHandleHelper, TYP_REF,
                                              op1->AsCall()->gtArgs.GetArgByIndex(0)->GetEarlyNode());
                    op1->gtType = TYP_REF;
                    retNode     = op1;
                }
                break;
            }

            case NI_System_Type_op_Equality:
            case NI_System_Type_op_Inequality:
            {
                JITDUMP("Importing Type.op_*Equality intrinsic\n");
                GenTree* op1     = impStackTop(1).val;
                GenTree* op2     = impStackTop(0).val;
                GenTree* optTree = gtFoldTypeEqualityCall(ni == NI_System_Type_op_Equality, op1, op2);
                if (optTree != nullptr)
                {
                    // Success, clean up the evaluation stack.
                    impPopStack();
                    impPopStack();

                    // See if we can optimize even further, to a handle compare.
                    optTree = gtFoldTypeCompare(optTree);

                    // See if we can now fold a handle compare to a constant.
                    optTree = gtFoldExpr(optTree);

                    retNode = optTree;
                }
                else
                {
                    // Retry optimizing these later
                    isSpecial = true;
                }
                break;
            }

            case NI_System_Enum_HasFlag:
            {
                GenTree* thisOp  = impStackTop(1).val;
                GenTree* flagOp  = impStackTop(0).val;
                GenTree* optTree = gtOptimizeEnumHasFlag(thisOp, flagOp);

                if (optTree != nullptr)
                {
                    // Optimization successful. Pop the stack for real.
                    impPopStack();
                    impPopStack();
                    retNode = optTree;
                }
                else
                {
                    // Retry optimizing this during morph.
                    isSpecial = true;
                }

                break;
            }

            case NI_System_Type_IsAssignableFrom:
            {
                GenTree* typeTo   = impStackTop(1).val;
                GenTree* typeFrom = impStackTop(0).val;

                retNode = impTypeIsAssignable(typeTo, typeFrom);
                break;
            }

            case NI_System_Type_IsAssignableTo:
            {
                GenTree* typeTo   = impStackTop(0).val;
                GenTree* typeFrom = impStackTop(1).val;

                retNode = impTypeIsAssignable(typeTo, typeFrom);
                break;
            }

            case NI_System_Type_get_IsValueType:
            case NI_System_Type_get_IsByRefLike:
            {
                // Optimize
                //
                //   call Type.GetTypeFromHandle (which is replaced with CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE)
                //   call Type.IsXXX
                //
                // to `true` or `false`
                // e.g., `typeof(int).IsValueType` => `true`
                // e.g., `typeof(Span<int>).IsByRefLike` => `true`
                if (impStackTop().val->IsCall())
                {
                    GenTreeCall* call = impStackTop().val->AsCall();
                    if (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE))
                    {
                        assert(call->gtArgs.CountArgs() == 1);
                        CORINFO_CLASS_HANDLE hClass =
                            gtGetHelperArgClassHandle(call->gtArgs.GetArgByIndex(0)->GetEarlyNode());
                        if (hClass != NO_CLASS_HANDLE)
                        {
                            switch (ni)
                            {
                                case NI_System_Type_get_IsValueType:
                                    retNode = gtNewIconNode(eeIsValueClass(hClass) ? 1 : 0);
                                    break;
                                case NI_System_Type_get_IsByRefLike:
                                    retNode = gtNewIconNode(
                                        (info.compCompHnd->getClassAttribs(hClass) & CORINFO_FLG_BYREF_LIKE) ? 1 : 0);
                                    break;
                                default:
                                    NO_WAY("Intrinsic not supported in this path.");
                            }
                            impPopStack(); // drop CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE call
                        }
                    }
                }
                break;
            }

            case NI_System_Threading_Thread_get_ManagedThreadId:
            {
                if (impStackTop().val->OperIs(GT_RET_EXPR))
                {
                    GenTreeCall* call = impStackTop().val->AsRetExpr()->gtInlineCandidate->AsCall();
                    if (call->gtCallMoreFlags & GTF_CALL_M_SPECIAL_INTRINSIC)
                    {
                        if (lookupNamedIntrinsic(call->gtCallMethHnd) == NI_System_Threading_Thread_get_CurrentThread)
                        {
                            // drop get_CurrentThread() call
                            impPopStack();
                            call->ReplaceWith(gtNewNothingNode(), this);
                            retNode = gtNewHelperCallNode(CORINFO_HELP_GETCURRENTMANAGEDTHREADID, TYP_INT);
                        }
                    }
                }
                break;
            }

#ifdef TARGET_ARM64
            // Intrinsify Interlocked.Or and Interlocked.And only for arm64-v8.1 (and newer)
            // TODO-CQ: Implement for XArch (https://github.com/dotnet/runtime/issues/32239).
            case NI_System_Threading_Interlocked_Or:
            case NI_System_Threading_Interlocked_And:
            {
                if (compOpportunisticallyDependsOn(InstructionSet_Atomics))
                {
                    assert(sig->numArgs == 2);
                    GenTree*   op2 = impPopStack().val;
                    GenTree*   op1 = impPopStack().val;
                    genTreeOps op  = (ni == NI_System_Threading_Interlocked_Or) ? GT_XORR : GT_XAND;
                    retNode        = gtNewOperNode(op, genActualType(callType), op1, op2);
                    retNode->gtFlags |= GTF_GLOB_REF | GTF_ASG;
                }
                break;
            }
#endif // TARGET_ARM64

#if defined(TARGET_XARCH) || defined(TARGET_ARM64)
            // TODO-ARM-CQ: reenable treating InterlockedCmpXchg32 operation as intrinsic
            case NI_System_Threading_Interlocked_CompareExchange:
            {
                var_types retType = JITtype2varType(sig->retType);
                if ((retType == TYP_LONG) && (TARGET_POINTER_SIZE == 4))
                {
                    break;
                }
                if ((retType != TYP_INT) && (retType != TYP_LONG))
                {
                    break;
                }

                assert(callType != TYP_STRUCT);
                assert(sig->numArgs == 3);

                GenTree* op3 = impPopStack().val; // comparand
                GenTree* op2 = impPopStack().val; // value
                GenTree* op1 = impPopStack().val; // location

                GenTree* node = new (this, GT_CMPXCHG) GenTreeCmpXchg(genActualType(callType), op1, op2, op3);

                node->AsCmpXchg()->gtOpLocation->gtFlags |= GTF_DONT_CSE;
                retNode = node;
                break;
            }

            case NI_System_Threading_Interlocked_Exchange:
            case NI_System_Threading_Interlocked_ExchangeAdd:
            {
                assert(callType != TYP_STRUCT);
                assert(sig->numArgs == 2);

                var_types retType = JITtype2varType(sig->retType);
                if ((retType == TYP_LONG) && (TARGET_POINTER_SIZE == 4))
                {
                    break;
                }
                if ((retType != TYP_INT) && (retType != TYP_LONG))
                {
                    break;
                }

                GenTree* op2 = impPopStack().val;
                GenTree* op1 = impPopStack().val;

                // This creates:
                //   val
                // XAdd
                //   addr
                //     field (for example)
                //
                // In the case where the first argument is the address of a local, we might
                // want to make this *not* make the var address-taken -- but atomic instructions
                // on a local are probably pretty useless anyway, so we probably don't care.

                op1 = gtNewOperNode(ni == NI_System_Threading_Interlocked_ExchangeAdd ? GT_XADD : GT_XCHG,
                                    genActualType(callType), op1, op2);
                op1->gtFlags |= GTF_GLOB_REF | GTF_ASG;
                retNode = op1;
                break;
            }
#endif // defined(TARGET_XARCH) || defined(TARGET_ARM64)

            case NI_System_Threading_Interlocked_MemoryBarrier:
            case NI_System_Threading_Interlocked_ReadMemoryBarrier:
            {
                assert(sig->numArgs == 0);

                GenTree* op1 = new (this, GT_MEMORYBARRIER) GenTree(GT_MEMORYBARRIER, TYP_VOID);
                op1->gtFlags |= GTF_GLOB_REF | GTF_ASG;

                // On XARCH `NI_System_Threading_Interlocked_ReadMemoryBarrier` fences need not be emitted.
                // However, we still need to capture the effect on reordering.
                if (ni == NI_System_Threading_Interlocked_ReadMemoryBarrier)
                {
                    op1->gtFlags |= GTF_MEMORYBARRIER_LOAD;
                }

                retNode = op1;
                break;
            }

#ifdef FEATURE_HW_INTRINSICS
            case NI_System_Math_FusedMultiplyAdd:
            {
#ifdef TARGET_XARCH
                if (compExactlyDependsOn(InstructionSet_FMA))
                {
                    assert(varTypeIsFloating(callType));

                    // We are constructing a chain of intrinsics similar to:
                    //    return FMA.MultiplyAddScalar(
                    //        Vector128.CreateScalarUnsafe(x),
                    //        Vector128.CreateScalarUnsafe(y),
                    //        Vector128.CreateScalarUnsafe(z)
                    //    ).ToScalar();

                    GenTree* op3 = gtNewSimdHWIntrinsicNode(TYP_SIMD16, impPopStack().val,
                                                            NI_Vector128_CreateScalarUnsafe, callJitType, 16);
                    GenTree* op2 = gtNewSimdHWIntrinsicNode(TYP_SIMD16, impPopStack().val,
                                                            NI_Vector128_CreateScalarUnsafe, callJitType, 16);
                    GenTree* op1 = gtNewSimdHWIntrinsicNode(TYP_SIMD16, impPopStack().val,
                                                            NI_Vector128_CreateScalarUnsafe, callJitType, 16);
                    GenTree* res =
                        gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, op2, op3, NI_FMA_MultiplyAddScalar, callJitType, 16);

                    retNode = gtNewSimdHWIntrinsicNode(callType, res, NI_Vector128_ToScalar, callJitType, 16);
                    break;
                }
#elif defined(TARGET_ARM64)
                if (compExactlyDependsOn(InstructionSet_AdvSimd))
                {
                    assert(varTypeIsFloating(callType));

                    // We are constructing a chain of intrinsics similar to:
                    //    return AdvSimd.FusedMultiplyAddScalar(
                    //        Vector64.Create{ScalarUnsafe}(z),
                    //        Vector64.Create{ScalarUnsafe}(y),
                    //        Vector64.Create{ScalarUnsafe}(x)
                    //    ).ToScalar();

                    NamedIntrinsic createVector64 =
                        (callType == TYP_DOUBLE) ? NI_Vector64_Create : NI_Vector64_CreateScalarUnsafe;

                    constexpr unsigned int simdSize = 8;

                    GenTree* op3 =
                        gtNewSimdHWIntrinsicNode(TYP_SIMD8, impPopStack().val, createVector64, callJitType, simdSize);
                    GenTree* op2 =
                        gtNewSimdHWIntrinsicNode(TYP_SIMD8, impPopStack().val, createVector64, callJitType, simdSize);
                    GenTree* op1 =
                        gtNewSimdHWIntrinsicNode(TYP_SIMD8, impPopStack().val, createVector64, callJitType, simdSize);

                    // Note that AdvSimd.FusedMultiplyAddScalar(op1,op2,op3) corresponds to op1 + op2 * op3
                    // while Math{F}.FusedMultiplyAddScalar(op1,op2,op3) corresponds to op1 * op2 + op3
                    retNode = gtNewSimdHWIntrinsicNode(TYP_SIMD8, op3, op2, op1, NI_AdvSimd_FusedMultiplyAddScalar,
                                                       callJitType, simdSize);

                    retNode = gtNewSimdHWIntrinsicNode(callType, retNode, NI_Vector64_ToScalar, callJitType, simdSize);
                    break;
                }
#endif

                // TODO-CQ-XArch: Ideally we would create a GT_INTRINSIC node for fma, however, that currently
                // requires more extensive changes to valuenum to support methods with 3 operands

                // We want to generate a GT_INTRINSIC node in the case the call can't be treated as
                // a target intrinsic so that we can still benefit from CSE and constant folding.

                break;
            }
#endif // FEATURE_HW_INTRINSICS

            case NI_System_Math_Abs:
            case NI_System_Math_Acos:
            case NI_System_Math_Acosh:
            case NI_System_Math_Asin:
            case NI_System_Math_Asinh:
            case NI_System_Math_Atan:
            case NI_System_Math_Atanh:
            case NI_System_Math_Atan2:
            case NI_System_Math_Cbrt:
            case NI_System_Math_Ceiling:
            case NI_System_Math_Cos:
            case NI_System_Math_Cosh:
            case NI_System_Math_Exp:
            case NI_System_Math_Floor:
            case NI_System_Math_FMod:
            case NI_System_Math_ILogB:
            case NI_System_Math_Log:
            case NI_System_Math_Log2:
            case NI_System_Math_Log10:
            {
                retNode = impMathIntrinsic(method, sig, callType, ni, tailCall);
                break;
            }
#if defined(TARGET_ARM64)
            // ARM64 has fmax/fmin which are IEEE754:2019 minimum/maximum compatible
            // TODO-XARCH-CQ: Enable this for XARCH when one of the arguments is a constant
            // so we can then emit maxss/minss and avoid NaN/-0.0 handling
            case NI_System_Math_Max:
            case NI_System_Math_Min:
#endif

#if defined(FEATURE_HW_INTRINSICS) && defined(TARGET_XARCH)
            case NI_System_Math_Max:
            case NI_System_Math_Min:
            {
                assert(varTypeIsFloating(callType));
                assert(sig->numArgs == 2);

                GenTreeDblCon* cnsNode   = nullptr;
                GenTree*       otherNode = nullptr;

                GenTree* op2 = impStackTop().val;
                GenTree* op1 = impStackTop(1).val;

                if (op2->IsCnsFltOrDbl())
                {
                    cnsNode   = op2->AsDblCon();
                    otherNode = op1;
                }
                else if (op1->IsCnsFltOrDbl())
                {
                    cnsNode   = op1->AsDblCon();
                    otherNode = op2;
                }

                if (cnsNode == nullptr)
                {
                    // no constant node, nothing to do
                    break;
                }

                if (otherNode->IsCnsFltOrDbl())
                {
                    // both are constant, we can fold this operation completely. Pop both peeked values

                    if (ni == NI_System_Math_Max)
                    {
                        cnsNode->SetDconValue(
                            FloatingPointUtils::maximum(cnsNode->DconValue(), otherNode->AsDblCon()->DconValue()));
                    }
                    else
                    {
                        assert(ni == NI_System_Math_Min);
                        cnsNode->SetDconValue(
                            FloatingPointUtils::minimum(cnsNode->DconValue(), otherNode->AsDblCon()->DconValue()));
                    }

                    retNode = cnsNode;

                    impPopStack();
                    impPopStack();
                    DEBUG_DESTROY_NODE(otherNode);

                    break;
                }

                // only one is constant, we can fold in specialized scenarios

                if (cnsNode->IsFloatNaN())
                {
                    impSpillSideEffects(false, CHECK_SPILL_ALL DEBUGARG("spill side effects before propagating NaN"));

                    // maxsd, maxss, minsd, and minss all return op2 if either is NaN
                    // we require NaN to be propagated so ensure the known NaN is op2

                    impPopStack();
                    impPopStack();
                    DEBUG_DESTROY_NODE(otherNode);

                    retNode = cnsNode;
                    break;
                }

                if (!compOpportunisticallyDependsOn(InstructionSet_SSE2))
                {
                    break;
                }

                if (ni == NI_System_Math_Max)
                {
                    // maxsd, maxss return op2 if both inputs are 0 of either sign
                    // we require +0 to be greater than -0, so we can't handle if
                    // the known constant is +0. This is because if the unknown value
                    // is -0, we'd need the cns to be op2. But if the unknown value
                    // is NaN, we'd need the cns to be op1 instead.

                    if (cnsNode->IsFloatPositiveZero())
                    {
                        break;
                    }

                    // Given the checks, op1 can safely be the cns and op2 the other node

                    ni = (callType == TYP_DOUBLE) ? NI_SSE2_Max : NI_SSE_Max;

                    // one is constant and we know its something we can handle, so pop both peeked values

                    op1 = cnsNode;
                    op2 = otherNode;
                }
                else
                {
                    assert(ni == NI_System_Math_Min);

                    // minsd, minss return op2 if both inputs are 0 of either sign
                    // we require -0 to be lesser than +0, so we can't handle if
                    // the known constant is -0. This is because if the unknown value
                    // is +0, we'd need the cns to be op2. But if the unknown value
                    // is NaN, we'd need the cns to be op1 instead.

                    if (cnsNode->IsFloatNegativeZero())
                    {
                        break;
                    }

                    // Given the checks, op1 can safely be the cns and op2 the other node

                    ni = (callType == TYP_DOUBLE) ? NI_SSE2_Min : NI_SSE_Min;

                    // one is constant and we know its something we can handle, so pop both peeked values

                    op1 = cnsNode;
                    op2 = otherNode;
                }

                assert(op1->IsCnsFltOrDbl() && !op2->IsCnsFltOrDbl());

                impPopStack();
                impPopStack();

                GenTreeVecCon* vecCon = gtNewVconNode(TYP_SIMD16);

                if (callJitType == CORINFO_TYPE_FLOAT)
                {
                    vecCon->gtSimd16Val.f32[0] = (float)op1->AsDblCon()->DconValue();
                }
                else
                {
                    vecCon->gtSimd16Val.f64[0] = op1->AsDblCon()->DconValue();
                }

                op1 = vecCon;
                op2 = gtNewSimdHWIntrinsicNode(TYP_SIMD16, op2, NI_Vector128_CreateScalarUnsafe, callJitType, 16);

                retNode = gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, op2, ni, callJitType, 16);
                retNode = gtNewSimdHWIntrinsicNode(callType, retNode, NI_Vector128_ToScalar, callJitType, 16);

                break;
            }
#endif

            case NI_System_Math_Pow:
            case NI_System_Math_Round:
            case NI_System_Math_Sin:
            case NI_System_Math_Sinh:
            case NI_System_Math_Sqrt:
            case NI_System_Math_Tan:
            case NI_System_Math_Tanh:
            case NI_System_Math_Truncate:
            {
                retNode = impMathIntrinsic(method, sig, callType, ni, tailCall);
                break;
            }

            case NI_System_Array_Clone:
            case NI_System_Collections_Generic_Comparer_get_Default:
            case NI_System_Collections_Generic_EqualityComparer_get_Default:
            case NI_System_Object_MemberwiseClone:
            case NI_System_Threading_Thread_get_CurrentThread:
            {
                // Flag for later handling.
                isSpecial = true;
                break;
            }

            case NI_System_Object_GetType:
            {
                JITDUMP("\n impIntrinsic: call to Object.GetType\n");
                GenTree* op1 = impStackTop(0).val;

                // If we're calling GetType on a boxed value, just get the type directly.
                if (op1->IsBoxedValue())
                {
                    JITDUMP("Attempting to optimize box(...).getType() to direct type construction\n");

                    // Try and clean up the box. Obtain the handle we
                    // were going to pass to the newobj.
                    GenTree* boxTypeHandle = gtTryRemoveBoxUpstreamEffects(op1, BR_REMOVE_AND_NARROW_WANT_TYPE_HANDLE);

                    if (boxTypeHandle != nullptr)
                    {
                        // Note we don't need to play the TYP_STRUCT games here like
                        // do for LDTOKEN since the return value of this operator is Type,
                        // not RuntimeTypeHandle.
                        impPopStack();
                        GenTree* runtimeType =
                            gtNewHelperCallNode(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE, TYP_REF, boxTypeHandle);
                        retNode = runtimeType;
                    }
                }

                // If we have a constrained callvirt with a "box this" transform
                // we know we have a value class and hence an exact type.
                //
                // If so, instead of boxing and then extracting the type, just
                // construct the type directly.
                if ((retNode == nullptr) && (pConstrainedResolvedToken != nullptr) &&
                    (constraintCallThisTransform == CORINFO_BOX_THIS))
                {
                    // Ensure this is one of the simple box cases (in particular, rule out nullables).
                    const CorInfoHelpFunc boxHelper = info.compCompHnd->getBoxHelper(pConstrainedResolvedToken->hClass);
                    const bool            isSafeToOptimize = (boxHelper == CORINFO_HELP_BOX);

                    if (isSafeToOptimize)
                    {
                        JITDUMP("Optimizing constrained box-this obj.getType() to direct type construction\n");
                        impPopStack();
                        GenTree* typeHandleOp =
                            impTokenToHandle(pConstrainedResolvedToken, nullptr, true /* mustRestoreHandle */);
                        if (typeHandleOp == nullptr)
                        {
                            assert(compDonotInline());
                            return nullptr;
                        }
                        GenTree* runtimeType =
                            gtNewHelperCallNode(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE, TYP_REF, typeHandleOp);
                        retNode = runtimeType;
                    }
                }

#ifdef DEBUG
                if (retNode != nullptr)
                {
                    JITDUMP("Optimized result for call to GetType is\n");
                    if (verbose)
                    {
                        gtDispTree(retNode);
                    }
                }
#endif

                // Else expand as an intrinsic, unless the call is constrained,
                // in which case we defer expansion to allow impImportCall do the
                // special constraint processing.
                if ((retNode == nullptr) && (pConstrainedResolvedToken == nullptr))
                {
                    JITDUMP("Expanding as special intrinsic\n");
                    impPopStack();
                    op1 = new (this, GT_INTRINSIC) GenTreeIntrinsic(genActualType(callType), op1, ni, method);

                    // Set the CALL flag to indicate that the operator is implemented by a call.
                    // Set also the EXCEPTION flag because the native implementation of
                    // NI_System_Object_GetType intrinsic can throw NullReferenceException.
                    op1->gtFlags |= (GTF_CALL | GTF_EXCEPT);
                    retNode = op1;
                    // Might be further optimizable, so arrange to leave a mark behind
                    isSpecial = true;
                }

                if (retNode == nullptr)
                {
                    JITDUMP("Leaving as normal call\n");
                    // Might be further optimizable, so arrange to leave a mark behind
                    isSpecial = true;
                }

                break;
            }

            case NI_System_Array_GetLength:
            case NI_System_Array_GetLowerBound:
            case NI_System_Array_GetUpperBound:
            {
                // System.Array.GetLength(Int32) method:
                //     public int GetLength(int dimension)
                // System.Array.GetLowerBound(Int32) method:
                //     public int GetLowerBound(int dimension)
                // System.Array.GetUpperBound(Int32) method:
                //     public int GetUpperBound(int dimension)
                //
                // Only implement these as intrinsics for multi-dimensional arrays.
                // Only handle constant dimension arguments.

                GenTree* gtDim = impStackTop().val;
                GenTree* gtArr = impStackTop(1).val;

                if (gtDim->IsIntegralConst())
                {
                    bool                 isExact   = false;
                    bool                 isNonNull = false;
                    CORINFO_CLASS_HANDLE arrCls    = gtGetClassHandle(gtArr, &isExact, &isNonNull);
                    if (arrCls != NO_CLASS_HANDLE)
                    {
                        unsigned rank = info.compCompHnd->getArrayRank(arrCls);
                        if ((rank > 1) && !info.compCompHnd->isSDArray(arrCls))
                        {
                            // `rank` is guaranteed to be <=32 (see MAX_RANK in vm\array.h). Any constant argument
                            // is `int` sized.
                            INT64 dimValue = gtDim->AsIntConCommon()->IntegralValue();
                            assert((unsigned int)dimValue == dimValue);
                            unsigned dim = (unsigned int)dimValue;
                            if (dim < rank)
                            {
                                // This is now known to be a multi-dimension array with a constant dimension
                                // that is in range; we can expand it as an intrinsic.

                                impPopStack().val; // Pop the dim and array object; we already have a pointer to them.
                                impPopStack().val;

                                // Make sure there are no global effects in the array (such as it being a function
                                // call), so we can mark the generated indirection with GTF_IND_INVARIANT. In the
                                // GetUpperBound case we need the cloned object, since we refer to the array
                                // object twice. In the other cases, we don't need to clone.
                                GenTree* gtArrClone = nullptr;
                                if (((gtArr->gtFlags & GTF_GLOB_EFFECT) != 0) || (ni == NI_System_Array_GetUpperBound))
                                {
                                    gtArr = impCloneExpr(gtArr, &gtArrClone, NO_CLASS_HANDLE, CHECK_SPILL_ALL,
                                                         nullptr DEBUGARG("MD intrinsics array"));
                                }

                                switch (ni)
                                {
                                    case NI_System_Array_GetLength:
                                    {
                                        // Generate *(array + offset-to-length-array + sizeof(int) * dim)
                                        unsigned offs   = eeGetMDArrayLengthOffset(rank, dim);
                                        GenTree* gtOffs = gtNewIconNode(offs, TYP_I_IMPL);
                                        GenTree* gtAddr = gtNewOperNode(GT_ADD, TYP_BYREF, gtArr, gtOffs);
                                        retNode         = gtNewIndir(TYP_INT, gtAddr);
                                        retNode->gtFlags |= GTF_IND_INVARIANT;
                                        break;
                                    }
                                    case NI_System_Array_GetLowerBound:
                                    {
                                        // Generate *(array + offset-to-bounds-array + sizeof(int) * dim)
                                        unsigned offs   = eeGetMDArrayLowerBoundOffset(rank, dim);
                                        GenTree* gtOffs = gtNewIconNode(offs, TYP_I_IMPL);
                                        GenTree* gtAddr = gtNewOperNode(GT_ADD, TYP_BYREF, gtArr, gtOffs);
                                        retNode         = gtNewIndir(TYP_INT, gtAddr);
                                        retNode->gtFlags |= GTF_IND_INVARIANT;
                                        break;
                                    }
                                    case NI_System_Array_GetUpperBound:
                                    {
                                        assert(gtArrClone != nullptr);

                                        // Generate:
                                        //    *(array + offset-to-length-array + sizeof(int) * dim) +
                                        //    *(array + offset-to-bounds-array + sizeof(int) * dim) - 1
                                        unsigned offs         = eeGetMDArrayLowerBoundOffset(rank, dim);
                                        GenTree* gtOffs       = gtNewIconNode(offs, TYP_I_IMPL);
                                        GenTree* gtAddr       = gtNewOperNode(GT_ADD, TYP_BYREF, gtArr, gtOffs);
                                        GenTree* gtLowerBound = gtNewIndir(TYP_INT, gtAddr);
                                        gtLowerBound->gtFlags |= GTF_IND_INVARIANT;

                                        offs              = eeGetMDArrayLengthOffset(rank, dim);
                                        gtOffs            = gtNewIconNode(offs, TYP_I_IMPL);
                                        gtAddr            = gtNewOperNode(GT_ADD, TYP_BYREF, gtArrClone, gtOffs);
                                        GenTree* gtLength = gtNewIndir(TYP_INT, gtAddr);
                                        gtLength->gtFlags |= GTF_IND_INVARIANT;

                                        GenTree* gtSum = gtNewOperNode(GT_ADD, TYP_INT, gtLowerBound, gtLength);
                                        GenTree* gtOne = gtNewIconNode(1, TYP_INT);
                                        retNode        = gtNewOperNode(GT_SUB, TYP_INT, gtSum, gtOne);
                                        break;
                                    }
                                    default:
                                        unreached();
                                }
                            }
                        }
                    }
                }
                break;
            }

            case NI_System_Buffers_Binary_BinaryPrimitives_ReverseEndianness:
            {
                assert(sig->numArgs == 1);

                // We expect the return type of the ReverseEndianness routine to match the type of the
                // one and only argument to the method. We use a special instruction for 16-bit
                // BSWAPs since on x86 processors this is implemented as ROR <16-bit reg>, 8. Additionally,
                // we only emit 64-bit BSWAP instructions on 64-bit archs; if we're asked to perform a
                // 64-bit byte swap on a 32-bit arch, we'll fall to the default case in the switch block below.

                switch (sig->retType)
                {
                    case CorInfoType::CORINFO_TYPE_SHORT:
                    case CorInfoType::CORINFO_TYPE_USHORT:
                        retNode = gtNewCastNode(TYP_INT, gtNewOperNode(GT_BSWAP16, TYP_INT, impPopStack().val), false,
                                                callType);
                        break;

                    case CorInfoType::CORINFO_TYPE_INT:
                    case CorInfoType::CORINFO_TYPE_UINT:
#ifdef TARGET_64BIT
                    case CorInfoType::CORINFO_TYPE_LONG:
                    case CorInfoType::CORINFO_TYPE_ULONG:
#endif // TARGET_64BIT
                        retNode = gtNewOperNode(GT_BSWAP, callType, impPopStack().val);
                        break;

                    default:
                        // This default case gets hit on 32-bit archs when a call to a 64-bit overload
                        // of ReverseEndianness is encountered. In that case we'll let JIT treat this as a standard
                        // method call, where the implementation decomposes the operation into two 32-bit
                        // bswap routines. If the input to the 64-bit function is a constant, then we rely
                        // on inlining + constant folding of 32-bit bswaps to effectively constant fold
                        // the 64-bit call site.
                        break;
                }

                break;
            }

            // Fold PopCount for constant input
            case NI_System_Numerics_BitOperations_PopCount:
            {
                assert(sig->numArgs == 1);
                if (impStackTop().val->IsIntegralConst())
                {
                    typeInfo argType = verParseArgSigToTypeInfo(sig, sig->args).NormaliseForStack();
                    INT64    cns     = impPopStack().val->AsIntConCommon()->IntegralValue();
                    if (argType.IsType(TI_LONG))
                    {
                        retNode = gtNewIconNode(genCountBits(cns), callType);
                    }
                    else
                    {
                        assert(argType.IsType(TI_INT));
                        retNode = gtNewIconNode(genCountBits(static_cast<unsigned>(cns)), callType);
                    }
                }
                break;
            }

            case NI_System_GC_KeepAlive:
            {
                retNode = impKeepAliveIntrinsic(impPopStack().val);
                break;
            }

            case NI_System_BitConverter_DoubleToInt64Bits:
            {
                GenTree* op1 = impStackTop().val;
                assert(varTypeIsFloating(op1));

                if (op1->IsCnsFltOrDbl())
                {
                    impPopStack();

                    double f64Cns = op1->AsDblCon()->DconValue();
                    retNode       = gtNewLconNode(*reinterpret_cast<int64_t*>(&f64Cns));
                }
#if TARGET_64BIT
                else
                {
                    // TODO-Cleanup: We should support this on 32-bit but it requires decomposition work
                    impPopStack();

                    if (op1->TypeGet() != TYP_DOUBLE)
                    {
                        op1 = gtNewCastNode(TYP_DOUBLE, op1, false, TYP_DOUBLE);
                    }
                    retNode = gtNewBitCastNode(TYP_LONG, op1);
                }
#endif
                break;
            }

            case NI_System_BitConverter_Int32BitsToSingle:
            {
                GenTree* op1 = impPopStack().val;
                assert(varTypeIsInt(op1));

                if (op1->IsIntegralConst())
                {
                    int32_t i32Cns = (int32_t)op1->AsIntConCommon()->IconValue();
                    retNode        = gtNewDconNode(*reinterpret_cast<float*>(&i32Cns), TYP_FLOAT);
                }
                else
                {
                    retNode = gtNewBitCastNode(TYP_FLOAT, op1);
                }
                break;
            }

            case NI_System_BitConverter_Int64BitsToDouble:
            {
                GenTree* op1 = impStackTop().val;
                assert(varTypeIsLong(op1));

                if (op1->IsIntegralConst())
                {
                    impPopStack();

                    int64_t i64Cns = op1->AsIntConCommon()->LngValue();
                    retNode        = gtNewDconNode(*reinterpret_cast<double*>(&i64Cns));
                }
#if TARGET_64BIT
                else
                {
                    // TODO-Cleanup: We should support this on 32-bit but it requires decomposition work
                    impPopStack();

                    retNode = gtNewBitCastNode(TYP_DOUBLE, op1);
                }
#endif
                break;
            }

            case NI_System_BitConverter_SingleToInt32Bits:
            {
                GenTree* op1 = impPopStack().val;
                assert(varTypeIsFloating(op1));

                if (op1->IsCnsFltOrDbl())
                {
                    float f32Cns = (float)op1->AsDblCon()->DconValue();
                    retNode      = gtNewIconNode(*reinterpret_cast<int32_t*>(&f32Cns));
                }
                else
                {
                    if (op1->TypeGet() != TYP_FLOAT)
                    {
                        op1 = gtNewCastNode(TYP_FLOAT, op1, false, TYP_FLOAT);
                    }
                    retNode = gtNewBitCastNode(TYP_INT, op1);
                }
                break;
            }

            default:
                break;
        }
    }

    if (mustExpand && (retNode == nullptr))
    {
        assert(!"Unhandled must expand intrinsic, throwing PlatformNotSupportedException");
        return impUnsupportedNamedIntrinsic(CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED, method, sig, mustExpand);
    }

    // Optionally report if this intrinsic is special
    // (that is, potentially re-optimizable during morph).
    if (isSpecialIntrinsic != nullptr)
    {
        *isSpecialIntrinsic = isSpecial;
    }

    return retNode;
}

GenTree* Compiler::impSRCSUnsafeIntrinsic(NamedIntrinsic        intrinsic,
                                          CORINFO_CLASS_HANDLE  clsHnd,
                                          CORINFO_METHOD_HANDLE method,
                                          CORINFO_SIG_INFO*     sig)
{
    // NextCallRetAddr requires a CALL, so return nullptr.
    if (info.compHasNextCallRetAddr)
    {
        return nullptr;
    }

    assert(sig->sigInst.classInstCount == 0);

    switch (intrinsic)
    {
        case NI_SRCS_UNSAFE_Add:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // sizeof !!T
            // conv.i
            // mul
            // add
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1, op2);

            op2 = impImplicitIorI4Cast(op2, TYP_I_IMPL);

            unsigned classSize = info.compCompHnd->getClassSize(sig->sigInst.methInst[0]);

            if (classSize != 1)
            {
                GenTree* size = gtNewIconNode(classSize, TYP_I_IMPL);
                op2           = gtNewOperNode(GT_MUL, TYP_I_IMPL, op2, size);
            }

            var_types type = impGetByRefResultType(GT_ADD, /* uns */ false, &op1, &op2);
            return gtNewOperNode(GT_ADD, type, op1, op2);
        }

        case NI_SRCS_UNSAFE_AddByteOffset:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // add
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1, op2);

            var_types type = impGetByRefResultType(GT_ADD, /* uns */ false, &op1, &op2);
            return gtNewOperNode(GT_ADD, type, op1, op2);
        }

        case NI_SRCS_UNSAFE_AreSame:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // ceq
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;

            GenTree* tmp = gtNewOperNode(GT_EQ, TYP_INT, op1, op2);
            return gtFoldExpr(tmp);
        }

        case NI_SRCS_UNSAFE_As:
        {
            assert((sig->sigInst.methInstCount == 1) || (sig->sigInst.methInstCount == 2));

            // ldarg.0
            // ret

            return impPopStack().val;
        }

        case NI_SRCS_UNSAFE_AsPointer:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // conv.u
            // ret

            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1);

            return gtNewCastNode(TYP_I_IMPL, op1, /* uns */ false, TYP_I_IMPL);
        }

        case NI_SRCS_UNSAFE_AsRef:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ret

            return impPopStack().val;
        }

        case NI_SRCS_UNSAFE_ByteOffset:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.1
            // ldarg.0
            // sub
            // ret

            impSpillSideEffect(true, verCurrentState.esStackDepth -
                                         2 DEBUGARG("Spilling op1 side effects for Unsafe.ByteOffset"));

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1, op2);

            var_types type = impGetByRefResultType(GT_SUB, /* uns */ false, &op2, &op1);
            return gtNewOperNode(GT_SUB, type, op2, op1);
        }

        case NI_SRCS_UNSAFE_Copy:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // ldobj !!T
            // stobj !!T
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_CopyBlock:
        {
            assert(sig->sigInst.methInstCount == 0);

            // ldarg.0
            // ldarg.1
            // ldarg.2
            // cpblk
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_CopyBlockUnaligned:
        {
            assert(sig->sigInst.methInstCount == 0);

            // ldarg.0
            // ldarg.1
            // ldarg.2
            // unaligned. 0x1
            // cpblk
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_InitBlock:
        {
            assert(sig->sigInst.methInstCount == 0);

            // ldarg.0
            // ldarg.1
            // ldarg.2
            // initblk
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_InitBlockUnaligned:
        {
            assert(sig->sigInst.methInstCount == 0);

            // ldarg.0
            // ldarg.1
            // ldarg.2
            // unaligned. 0x1
            // initblk
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_IsAddressGreaterThan:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // cgt.un
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;

            GenTree* tmp = gtNewOperNode(GT_GT, TYP_INT, op1, op2);
            tmp->gtFlags |= GTF_UNSIGNED;
            return gtFoldExpr(tmp);
        }

        case NI_SRCS_UNSAFE_IsAddressLessThan:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // clt.un
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;

            GenTree* tmp = gtNewOperNode(GT_LT, TYP_INT, op1, op2);
            tmp->gtFlags |= GTF_UNSIGNED;
            return gtFoldExpr(tmp);
        }

        case NI_SRCS_UNSAFE_IsNullRef:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldc.i4.0
            // conv.u
            // ceq
            // ret

            GenTree* op1 = impPopStack().val;
            GenTree* cns = gtNewIconNode(0, TYP_BYREF);
            GenTree* tmp = gtNewOperNode(GT_EQ, TYP_INT, op1, cns);
            return gtFoldExpr(tmp);
        }

        case NI_SRCS_UNSAFE_NullRef:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldc.i4.0
            // conv.u
            // ret

            return gtNewIconNode(0, TYP_BYREF);
        }

        case NI_SRCS_UNSAFE_Read:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldobj !!T
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_ReadUnaligned:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // unaligned. 0x1
            // ldobj !!T
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_SizeOf:
        {
            assert(sig->sigInst.methInstCount == 1);

            // sizeof !!T
            // ret

            unsigned classSize = info.compCompHnd->getClassSize(sig->sigInst.methInst[0]);
            return gtNewIconNode(classSize, TYP_INT);
        }

        case NI_SRCS_UNSAFE_SkipInit:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ret

            GenTree* op1 = impPopStack().val;

            if ((op1->gtFlags & GTF_SIDE_EFFECT) != 0)
            {
                return gtUnusedValNode(op1);
            }
            else
            {
                return gtNewNothingNode();
            }
        }

        case NI_SRCS_UNSAFE_Subtract:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // sizeof !!T
            // conv.i
            // mul
            // sub
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1, op2);

            op2 = impImplicitIorI4Cast(op2, TYP_I_IMPL);

            unsigned classSize = info.compCompHnd->getClassSize(sig->sigInst.methInst[0]);

            if (classSize != 1)
            {
                GenTree* size = gtNewIconNode(classSize, TYP_I_IMPL);
                op2           = gtNewOperNode(GT_MUL, TYP_I_IMPL, op2, size);
            }

            var_types type = impGetByRefResultType(GT_SUB, /* uns */ false, &op1, &op2);
            return gtNewOperNode(GT_SUB, type, op1, op2);
        }

        case NI_SRCS_UNSAFE_SubtractByteOffset:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // sub
            // ret

            GenTree* op2 = impPopStack().val;
            GenTree* op1 = impPopStack().val;
            impBashVarAddrsToI(op1, op2);

            var_types type = impGetByRefResultType(GT_SUB, /* uns */ false, &op1, &op2);
            return gtNewOperNode(GT_SUB, type, op1, op2);
        }

        case NI_SRCS_UNSAFE_Unbox:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // unbox !!T
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_Write:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // stobj !!T
            // ret

            return nullptr;
        }

        case NI_SRCS_UNSAFE_WriteUnaligned:
        {
            assert(sig->sigInst.methInstCount == 1);

            // ldarg.0
            // ldarg.1
            // unaligned. 0x01
            // stobj !!T
            // ret

            return nullptr;
        }

        default:
        {
            unreached();
        }
    }
}

//------------------------------------------------------------------------
// impPopCallArgs:
//   Pop the given number of values from the stack and return a list node with
//   their values.
//
// Parameters:
//   sig     - Signature used to figure out classes the runtime must load, and
//             also to record exact receiving argument types that may be needed for ABI
//             purposes later.
//   call    - The call to pop arguments into.
//
void Compiler::impPopCallArgs(CORINFO_SIG_INFO* sig, GenTreeCall* call)
{
    assert(call->gtArgs.IsEmpty());

    if (impStackHeight() < sig->numArgs)
    {
        BADCODE("not enough arguments for call");
    }

    struct SigParamInfo
    {
        CorInfoType          CorType;
        CORINFO_CLASS_HANDLE ClassHandle;
    };

    SigParamInfo  inlineParams[16];
    SigParamInfo* params = sig->numArgs <= 16 ? inlineParams : new (this, CMK_CallArgs) SigParamInfo[sig->numArgs];

    // We will iterate and pop the args in reverse order as we sometimes need
    // to spill some args. However, we need signature information and the
    // JIT-EE interface only allows us to iterate the signature forwards. We
    // will collect the needed information here and at the same time notify the
    // EE that the signature types need to be loaded.
    CORINFO_ARG_LIST_HANDLE sigArg = sig->args;
    for (unsigned i = 0; i < sig->numArgs; i++)
    {
        params[i].CorType = strip(info.compCompHnd->getArgType(sig, sigArg, &params[i].ClassHandle));

        if (params[i].CorType != CORINFO_TYPE_CLASS && params[i].CorType != CORINFO_TYPE_BYREF &&
            params[i].CorType != CORINFO_TYPE_PTR && params[i].CorType != CORINFO_TYPE_VAR)
        {
            CORINFO_CLASS_HANDLE argRealClass = info.compCompHnd->getArgClass(sig, sigArg);
            if (argRealClass != nullptr)
            {
                // Make sure that all valuetypes (including enums) that we push are loaded.
                // This is to guarantee that if a GC is triggered from the prestub of this methods,
                // all valuetypes in the method signature are already loaded.
                // We need to be able to find the size of the valuetypes, but we cannot
                // do a class-load from within GC.
                info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(argRealClass);
            }
        }

        sigArg = info.compCompHnd->getArgNext(sigArg);
    }

    if ((sig->retTypeSigClass != nullptr) && (sig->retType != CORINFO_TYPE_CLASS) &&
        (sig->retType != CORINFO_TYPE_BYREF) && (sig->retType != CORINFO_TYPE_PTR) &&
        (sig->retType != CORINFO_TYPE_VAR))
    {
        // Make sure that all valuetypes (including enums) that we push are loaded.
        // This is to guarantee that if a GC is triggered from the prestub of this methods,
        // all valuetypes in the method signature are already loaded.
        // We need to be able to find the size of the valuetypes, but we cannot
        // do a class-load from within GC.
        info.compCompHnd->classMustBeLoadedBeforeCodeIsRun(sig->retTypeSigClass);
    }

    // Now create the arguments in reverse.
    for (unsigned i = sig->numArgs; i > 0; i--)
    {
        StackEntry se      = impPopStack();
        typeInfo   ti      = se.seTypeInfo;
        GenTree*   argNode = se.val;

        var_types            jitSigType = JITtype2varType(params[i - 1].CorType);
        CORINFO_CLASS_HANDLE classHnd   = params[i - 1].ClassHandle;

        if (!impCheckImplicitArgumentCoercion(jitSigType, argNode->TypeGet()))
        {
            BADCODE("the call argument has a type that can't be implicitly converted to the signature type");
        }

        if (varTypeIsStruct(argNode))
        {
            // Morph trees that aren't already OBJs or MKREFANY to be OBJs
            assert(ti.IsType(TI_STRUCT));

            JITDUMP("Calling impNormStructVal on:\n");
            DISPTREE(argNode);

            argNode = impNormStructVal(argNode, classHnd, CHECK_SPILL_ALL);
            // For SIMD types the normalization can normalize TYP_STRUCT to
            // e.g. TYP_SIMD16 which we keep (along with the class handle) in
            // the CallArgs.
            jitSigType = argNode->TypeGet();

            JITDUMP("resulting tree:\n");
            DISPTREE(argNode);
        }
        else
        {
            // insert implied casts (from float to double or double to float)
            if ((jitSigType == TYP_DOUBLE) && argNode->TypeIs(TYP_FLOAT))
            {
                argNode = gtNewCastNode(TYP_DOUBLE, argNode, false, TYP_DOUBLE);
            }
            else if ((jitSigType == TYP_FLOAT) && argNode->TypeIs(TYP_DOUBLE))
            {
                argNode = gtNewCastNode(TYP_FLOAT, argNode, false, TYP_FLOAT);
            }

            // insert any widening or narrowing casts for backwards compatibility
            argNode = impImplicitIorI4Cast(argNode, jitSigType);
        }

        NewCallArg arg;
        if (varTypeIsStruct(jitSigType))
        {
            arg = NewCallArg::Struct(argNode, jitSigType, classHnd);
        }
        else
        {
            arg = NewCallArg::Primitive(argNode, jitSigType);
        }

        call->gtArgs.PushFront(this, arg);
        call->gtFlags |= argNode->gtFlags & GTF_GLOB_EFFECT;
    }
}

/*****************************************************************************
 *
 *  Pop the given number of values from the stack in reverse order (STDCALL/CDECL etc.)
 *  The first "skipReverseCount" items are not reversed.
 */

void Compiler::impPopReverseCallArgs(CORINFO_SIG_INFO* sig, GenTreeCall* call, unsigned skipReverseCount)
{
    assert(skipReverseCount <= sig->numArgs);

    impPopCallArgs(sig, call);

    call->gtArgs.Reverse(skipReverseCount, sig->numArgs - skipReverseCount);
}

GenTree* Compiler::impTransformThis(GenTree*                thisPtr,
                                    CORINFO_RESOLVED_TOKEN* pConstrainedResolvedToken,
                                    CORINFO_THIS_TRANSFORM  transform)
{
    switch (transform)
    {
        case CORINFO_DEREF_THIS:
        {
            GenTree* obj = thisPtr;

            // This does a LDIND on the obj, which should be a byref. pointing to a ref
            impBashVarAddrsToI(obj);
            assert(genActualType(obj->gtType) == TYP_I_IMPL || obj->gtType == TYP_BYREF);
            CorInfoType constraintTyp = info.compCompHnd->asCorInfoType(pConstrainedResolvedToken->hClass);

            obj = gtNewOperNode(GT_IND, JITtype2varType(constraintTyp), obj);
            // ldind could point anywhere, example a boxed class static int
            obj->gtFlags |= (GTF_EXCEPT | GTF_GLOB_REF);

            return obj;
        }

        case CORINFO_BOX_THIS:
        {
            // Constraint calls where there might be no
            // unboxed entry point require us to implement the call via helper.
            // These only occur when a possible target of the call
            // may have inherited an implementation of an interface
            // method from System.Object or System.ValueType.  The EE does not provide us with
            // "unboxed" versions of these methods.

            GenTree* obj = thisPtr;

            assert(obj->TypeGet() == TYP_BYREF || obj->TypeGet() == TYP_I_IMPL);
            obj = gtNewObjNode(pConstrainedResolvedToken->hClass, obj);
            obj->gtFlags |= GTF_EXCEPT;

            CorInfoType jitTyp = info.compCompHnd->asCorInfoType(pConstrainedResolvedToken->hClass);
            if (impIsPrimitive(jitTyp))
            {
                if (obj->OperIsBlk())
                {
                    obj->ChangeOperUnchecked(GT_IND);
                    obj->AsOp()->gtOp2 = nullptr; // must be zero for tree walkers
                }

                obj->gtType = JITtype2varType(jitTyp);
                assert(varTypeIsArithmetic(obj->gtType));
            }

            // This pushes on the dereferenced byref
            // This is then used immediately to box.
            impPushOnStack(obj, verMakeTypeInfo(pConstrainedResolvedToken->hClass).NormaliseForStack());

            // This pops off the byref-to-a-value-type remaining on the stack and
            // replaces it with a boxed object.
            // This is then used as the object to the virtual call immediately below.
            impImportAndPushBox(pConstrainedResolvedToken);
            if (compDonotInline())
            {
                return nullptr;
            }

            obj = impPopStack().val;
            return obj;
        }
        case CORINFO_NO_THIS_TRANSFORM:
        default:
            return thisPtr;
    }
}

//------------------------------------------------------------------------
// impCanPInvokeInline: check whether PInvoke inlining should enabled in current method.
//
// Return Value:
//    true if PInvoke inlining should be enabled in current method, false otherwise
//
// Notes:
//    Checks a number of ambient conditions where we could pinvoke but choose not to

bool Compiler::impCanPInvokeInline()
{
    return getInlinePInvokeEnabled() && (!opts.compDbgCode) && (compCodeOpt() != SMALL_CODE) &&
           (!opts.compNoPInvokeInlineCB) // profiler is preventing inline pinvoke
        ;
}

//------------------------------------------------------------------------
// impCanPInvokeInlineCallSite: basic legality checks using information
// from a call to see if the call qualifies as an inline pinvoke.
//
// Arguments:
//    block      - block containing the call, or for inlinees, block
//                 containing the call being inlined
//
// Return Value:
//    true if this call can legally qualify as an inline pinvoke, false otherwise
//
// Notes:
//    For runtimes that support exception handling interop there are
//    restrictions on using inline pinvoke in handler regions.
//
//    * We have to disable pinvoke inlining inside of filters because
//    in case the main execution (i.e. in the try block) is inside
//    unmanaged code, we cannot reuse the inlined stub (we still need
//    the original state until we are in the catch handler)
//
//    * We disable pinvoke inlining inside handlers since the GSCookie
//    is in the inlined Frame (see
//    CORINFO_EE_INFO::InlinedCallFrameInfo::offsetOfGSCookie), but
//    this would not protect framelets/return-address of handlers.
//
//    These restrictions are currently also in place for CoreCLR but
//    can be relaxed when coreclr/#8459 is addressed.

bool Compiler::impCanPInvokeInlineCallSite(BasicBlock* block)
{
    if (block->hasHndIndex())
    {
        return false;
    }

    // The remaining limitations do not apply to NativeAOT
    if (IsTargetAbi(CORINFO_NATIVEAOT_ABI))
    {
        return true;
    }

#ifdef USE_PER_FRAME_PINVOKE_INIT
    // For platforms that use per-P/Invoke InlinedCallFrame initialization,
    // we can't inline P/Invokes inside of try blocks where we can resume execution in the same function.
    // The runtime can correctly unwind out of an InlinedCallFrame and out of managed code. However,
    // it cannot correctly unwind out of an InlinedCallFrame and stop at that frame without also unwinding
    // at least one managed frame. In particular, the runtime struggles to restore non-volatile registers
    // from the top-most unmanaged call before the InlinedCallFrame. As a result, the runtime does not support
    // re-entering the same method frame as the InlinedCallFrame after an exception in unmanaged code.
    if (block->hasTryIndex())
    {
        // This does not apply to the raw pinvoke call that is inside the pinvoke
        // ILStub. In this case, we have to inline the raw pinvoke call into the stub,
        // otherwise we would end up with a stub that recursively calls itself, and end
        // up with a stack overflow.
        // This works correctly because the runtime never emits a catch block in a managed-to-native
        // IL stub. If the runtime ever emits a catch block into a managed-to-native stub when using
        // P/Invoke helpers, this condition will need to be revisited.
        if (opts.jitFlags->IsSet(JitFlags::JIT_FLAG_IL_STUB) && opts.ShouldUsePInvokeHelpers())
        {
            return true;
        }

        // Check if this block's try block or any containing try blocks have catch handlers.
        // If any of the containing try blocks have catch handlers,
        // we cannot inline a P/Invoke for reasons above. If the handler is a fault or finally handler,
        // we can inline a P/Invoke into this block in the try since the code will not resume execution
        // in the same method after throwing an exception if only fault or finally handlers are executed.
        for (unsigned int ehIndex = block->getTryIndex(); ehIndex != EHblkDsc::NO_ENCLOSING_INDEX;
             ehIndex              = ehGetEnclosingTryIndex(ehIndex))
        {
            if (ehGetDsc(ehIndex)->HasCatchHandler())
            {
                return false;
            }
        }

        return true;
    }
#endif // TARGET_64BIT

    return true;
}

//------------------------------------------------------------------------
// impCheckForPInvokeCall examine call to see if it is a pinvoke and if so
// if it can be expressed as an inline pinvoke.
//
// Arguments:
//    call       - tree for the call
//    methHnd    - handle for the method being called (may be null)
//    sig        - signature of the method being called
//    mflags     - method flags for the method being called
//    block      - block containing the call, or for inlinees, block
//                 containing the call being inlined
//
// Notes:
//   Sets GTF_CALL_M_PINVOKE on the call for pinvokes.
//
//   Also sets GTF_CALL_UNMANAGED on call for inline pinvokes if the
//   call passes a combination of legality and profitability checks.
//
//   If GTF_CALL_UNMANAGED is set, increments info.compUnmanagedCallCountWithGCTransition

void Compiler::impCheckForPInvokeCall(
    GenTreeCall* call, CORINFO_METHOD_HANDLE methHnd, CORINFO_SIG_INFO* sig, unsigned mflags, BasicBlock* block)
{
    CorInfoCallConvExtension unmanagedCallConv;

    // If VM flagged it as Pinvoke, flag the call node accordingly
    if ((mflags & CORINFO_FLG_PINVOKE) != 0)
    {
        call->gtCallMoreFlags |= GTF_CALL_M_PINVOKE;
    }

    bool suppressGCTransition = false;
    if (methHnd)
    {
        if ((mflags & CORINFO_FLG_PINVOKE) == 0)
        {
            return;
        }

        unmanagedCallConv = info.compCompHnd->getUnmanagedCallConv(methHnd, nullptr, &suppressGCTransition);
    }
    else
    {
        if (sig->getCallConv() == CORINFO_CALLCONV_DEFAULT || sig->getCallConv() == CORINFO_CALLCONV_VARARG)
        {
            return;
        }

        unmanagedCallConv = info.compCompHnd->getUnmanagedCallConv(nullptr, sig, &suppressGCTransition);

        assert(!call->gtCallCookie);
    }

    if (suppressGCTransition)
    {
        call->gtCallMoreFlags |= GTF_CALL_M_SUPPRESS_GC_TRANSITION;
    }

    if ((unmanagedCallConv == CorInfoCallConvExtension::Thiscall) && (sig->numArgs == 0))
    {
        BADCODE("thiscall with 0 arguments");
    }

    // If we can't get the unmanaged calling convention or the calling convention is unsupported in the JIT,
    // return here without inlining the native call.
    if (unmanagedCallConv == CorInfoCallConvExtension::Managed ||
        unmanagedCallConv == CorInfoCallConvExtension::Fastcall ||
        unmanagedCallConv == CorInfoCallConvExtension::FastcallMemberFunction)
    {
        return;
    }
    optNativeCallCount++;

    if (methHnd == nullptr && (opts.jitFlags->IsSet(JitFlags::JIT_FLAG_IL_STUB) || IsTargetAbi(CORINFO_NATIVEAOT_ABI)))
    {
        // PInvoke in NativeAOT ABI must be always inlined. Non-inlineable CALLI cases have been
        // converted to regular method calls earlier using convertPInvokeCalliToCall.

        // PInvoke CALLI in IL stubs must be inlined
    }
    else
    {
        // Check legality
        if (!impCanPInvokeInlineCallSite(block))
        {
            return;
        }

        // Legal PInvoke CALL in PInvoke IL stubs must be inlined to avoid infinite recursive
        // inlining in NativeAOT. Skip the ambient conditions checks and profitability checks.
        if (!IsTargetAbi(CORINFO_NATIVEAOT_ABI) || (info.compFlags & CORINFO_FLG_PINVOKE) == 0)
        {
            if (opts.jitFlags->IsSet(JitFlags::JIT_FLAG_IL_STUB) && opts.ShouldUsePInvokeHelpers())
            {
                // Raw PInvoke call in PInvoke IL stub generated must be inlined to avoid infinite
                // recursive calls to the stub.
            }
            else
            {
                if (!impCanPInvokeInline())
                {
                    return;
                }

                // Size-speed tradeoff: don't use inline pinvoke at rarely
                // executed call sites.  The non-inline version is more
                // compact.
                if (block->isRunRarely())
                {
                    return;
                }
            }
        }

        // The expensive check should be last
        if (info.compCompHnd->pInvokeMarshalingRequired(methHnd, sig))
        {
            return;
        }
    }

    JITLOG((LL_INFO1000000, "\nInline a CALLI PINVOKE call from method %s\n", info.compFullName));

    call->gtFlags |= GTF_CALL_UNMANAGED;
    call->unmgdCallConv = unmanagedCallConv;
    if (!call->IsSuppressGCTransition())
    {
        info.compUnmanagedCallCountWithGCTransition++;
    }

    // AMD64 convention is same for native and managed
    if (unmanagedCallConv == CorInfoCallConvExtension::C ||
        unmanagedCallConv == CorInfoCallConvExtension::CMemberFunction)
    {
        call->gtFlags |= GTF_CALL_POP_ARGS;
    }
}

//------------------------------------------------------------------------
// SpillRetExprHelper: iterate through arguments tree and spill ret_expr to local variables.
//
class SpillRetExprHelper
{
public:
    SpillRetExprHelper(Compiler* comp) : comp(comp)
    {
    }

    void StoreRetExprResultsInArgs(GenTreeCall* call)
    {
        for (CallArg& arg : call->gtArgs.Args())
        {
            comp->fgWalkTreePre(&arg.EarlyNodeRef(), SpillRetExprVisitor, this);
        }
    }

private:
    static Compiler::fgWalkResult SpillRetExprVisitor(GenTree** pTree, Compiler::fgWalkData* fgWalkPre)
    {
        assert((pTree != nullptr) && (*pTree != nullptr));
        GenTree* tree = *pTree;
        if ((tree->gtFlags & GTF_CALL) == 0)
        {
            // Trees with ret_expr are marked as GTF_CALL.
            return Compiler::WALK_SKIP_SUBTREES;
        }
        if (tree->OperGet() == GT_RET_EXPR)
        {
            SpillRetExprHelper* walker = static_cast<SpillRetExprHelper*>(fgWalkPre->pCallbackData);
            walker->StoreRetExprAsLocalVar(pTree);
        }
        return Compiler::WALK_CONTINUE;
    }

    void StoreRetExprAsLocalVar(GenTree** pRetExpr)
    {
        GenTree* retExpr = *pRetExpr;
        assert(retExpr->OperGet() == GT_RET_EXPR);
        const unsigned tmp = comp->lvaGrabTemp(true DEBUGARG("spilling ret_expr"));
        JITDUMP("Storing return expression [%06u] to a local var V%02u.\n", comp->dspTreeID(retExpr), tmp);
        comp->impAssignTempGen(tmp, retExpr, (unsigned)Compiler::CHECK_SPILL_NONE);
        *pRetExpr = comp->gtNewLclvNode(tmp, retExpr->TypeGet());

        if (retExpr->TypeGet() == TYP_REF)
        {
            assert(comp->lvaTable[tmp].lvSingleDef == 0);
            comp->lvaTable[tmp].lvSingleDef = 1;
            JITDUMP("Marked V%02u as a single def temp\n", tmp);

            bool                 isExact   = false;
            bool                 isNonNull = false;
            CORINFO_CLASS_HANDLE retClsHnd = comp->gtGetClassHandle(retExpr, &isExact, &isNonNull);
            if (retClsHnd != nullptr)
            {
                comp->lvaSetClass(tmp, retClsHnd, isExact);
            }
        }
    }

private:
    Compiler* comp;
};

//------------------------------------------------------------------------
// addFatPointerCandidate: mark the call and the method, that they have a fat pointer candidate.
//                         Spill ret_expr in the call node, because they can't be cloned.
//
// Arguments:
//    call - fat calli candidate
//
void Compiler::addFatPointerCandidate(GenTreeCall* call)
{
    JITDUMP("Marking call [%06u] as fat pointer candidate\n", dspTreeID(call));
    setMethodHasFatPointer();
    call->SetFatPointerCandidate();
    SpillRetExprHelper helper(this);
    helper.StoreRetExprResultsInArgs(call);
}

//------------------------------------------------------------------------
// pickGDV: Use profile information to pick a GDV candidate for a call site.
//
// Arguments:
//    call        - the call
//    ilOffset    - exact IL offset of the call
//    isInterface - whether or not the call target is defined on an interface
//    classGuess  - [out] the class to guess for (mutually exclusive with methodGuess)
//    methodGuess - [out] the method to guess for (mutually exclusive with classGuess)
//    likelihood  - [out] an estimate of the likelihood that the guess will succeed
//
void Compiler::pickGDV(GenTreeCall*           call,
                       IL_OFFSET              ilOffset,
                       bool                   isInterface,
                       CORINFO_CLASS_HANDLE*  classGuess,
                       CORINFO_METHOD_HANDLE* methodGuess,
                       unsigned*              likelihood)
{
    *classGuess  = NO_CLASS_HANDLE;
    *methodGuess = NO_METHOD_HANDLE;
    *likelihood  = 0;

    const int               maxLikelyClasses = 32;
    LikelyClassMethodRecord likelyClasses[maxLikelyClasses];
    unsigned                numberOfClasses = 0;
    if (call->IsVirtualStub() || call->IsVirtualVtable())
    {
        numberOfClasses =
            getLikelyClasses(likelyClasses, maxLikelyClasses, fgPgoSchema, fgPgoSchemaCount, fgPgoData, ilOffset);
    }

    const int               maxLikelyMethods = 32;
    LikelyClassMethodRecord likelyMethods[maxLikelyMethods];
    unsigned                numberOfMethods = 0;

    // TODO-GDV: R2R support requires additional work to reacquire the
    // entrypoint, similar to what happens at the end of impDevirtualizeCall.
    // As part of supporting this we should merge the tail of
    // impDevirtualizeCall and what happens in
    // GuardedDevirtualizationTransformer::CreateThen for method GDV.
    //
    if (!opts.IsReadyToRun() && (call->IsVirtualVtable() || call->IsDelegateInvoke()))
    {
        numberOfMethods =
            getLikelyMethods(likelyMethods, maxLikelyMethods, fgPgoSchema, fgPgoSchemaCount, fgPgoData, ilOffset);
    }

    if ((numberOfClasses < 1) && (numberOfMethods < 1))
    {
        JITDUMP("No likely class or method, sorry\n");
        return;
    }

#ifdef DEBUG
    if ((verbose || JitConfig.EnableExtraSuperPmiQueries()) && (numberOfClasses > 0))
    {
        bool                 isExact;
        bool                 isNonNull;
        CallArg*             thisArg            = call->gtArgs.GetThisArg();
        CORINFO_CLASS_HANDLE declaredThisClsHnd = gtGetClassHandle(thisArg->GetNode(), &isExact, &isNonNull);
        JITDUMP("Likely classes for call [%06u]", dspTreeID(call));
        if (declaredThisClsHnd != NO_CLASS_HANDLE)
        {
            const char* baseClassName = eeGetClassName(declaredThisClsHnd);
            JITDUMP(" on class %p (%s)", declaredThisClsHnd, baseClassName);
        }
        JITDUMP("\n");

        for (UINT32 i = 0; i < numberOfClasses; i++)
        {
            const char* className = eeGetClassName((CORINFO_CLASS_HANDLE)likelyClasses[i].handle);
            JITDUMP("  %u) %p (%s) [likelihood:%u%%]\n", i + 1, likelyClasses[i].handle, className,
                    likelyClasses[i].likelihood);
        }
    }

    if ((verbose || JitConfig.EnableExtraSuperPmiQueries()) && (numberOfMethods > 0))
    {
        assert(call->gtCallType == CT_USER_FUNC);
        const char* baseMethName = eeGetMethodFullName(call->gtCallMethHnd);
        JITDUMP("Likely methods for call [%06u] to method %s\n", dspTreeID(call), baseMethName);

        for (UINT32 i = 0; i < numberOfMethods; i++)
        {
            CORINFO_CONST_LOOKUP lookup = {};
            info.compCompHnd->getFunctionFixedEntryPoint((CORINFO_METHOD_HANDLE)likelyMethods[i].handle, false,
                                                         &lookup);

            const char* methName = eeGetMethodFullName((CORINFO_METHOD_HANDLE)likelyMethods[i].handle);
            switch (lookup.accessType)
            {
                case IAT_VALUE:
                    JITDUMP("  %u) %p (%s) [likelihood:%u%%]\n", i + 1, lookup.addr, methName,
                            likelyMethods[i].likelihood);
                    break;
                case IAT_PVALUE:
                    JITDUMP("  %u) [%p] (%s) [likelihood:%u%%]\n", i + 1, lookup.addr, methName,
                            likelyMethods[i].likelihood);
                    break;
                case IAT_PPVALUE:
                    JITDUMP("  %u) [[%p]] (%s) [likelihood:%u%%]\n", i + 1, lookup.addr, methName,
                            likelyMethods[i].likelihood);
                    break;
                default:
                    JITDUMP("  %u) %s [likelihood:%u%%]\n", i + 1, methName, likelyMethods[i].likelihood);
                    break;
            }
        }
    }

    // Optional stress mode to pick a random known class, rather than
    // the most likely known class.
    //
    if (JitConfig.JitRandomGuardedDevirtualization() != 0)
    {
        // Reuse the random inliner's random state.
        //
        CLRRandom* const random =
            impInlineRoot()->m_inlineStrategy->GetRandom(JitConfig.JitRandomGuardedDevirtualization());
        unsigned index = static_cast<unsigned>(random->Next(static_cast<int>(numberOfClasses + numberOfMethods)));
        if (index < numberOfClasses)
        {
            *classGuess = (CORINFO_CLASS_HANDLE)likelyClasses[index].handle;
            *likelihood = 100;
            JITDUMP("Picked random class for GDV: %p (%s)\n", *classGuess, eeGetClassName(*classGuess));
            return;
        }
        else
        {
            *methodGuess = (CORINFO_METHOD_HANDLE)likelyMethods[index - numberOfClasses].handle;
            *likelihood  = 100;
            JITDUMP("Picked random method for GDV: %p (%s)\n", *methodGuess, eeGetMethodFullName(*methodGuess));
            return;
        }
    }
#endif

    // Prefer class guess as it is cheaper
    if (numberOfClasses > 0)
    {
        unsigned likelihoodThreshold = isInterface ? 25 : 30;
        if (likelyClasses[0].likelihood >= likelihoodThreshold)
        {
            *classGuess = (CORINFO_CLASS_HANDLE)likelyClasses[0].handle;
            *likelihood = likelyClasses[0].likelihood;
            return;
        }

        JITDUMP("Not guessing for class; likelihood is below %s call threshold %u\n",
                isInterface ? "interface" : "virtual", likelihoodThreshold);
    }

    if (numberOfMethods > 0)
    {
        unsigned likelihoodThreshold = 30;
        if (likelyMethods[0].likelihood >= likelihoodThreshold)
        {
            *methodGuess = (CORINFO_METHOD_HANDLE)likelyMethods[0].handle;
            *likelihood  = likelyMethods[0].likelihood;
            return;
        }

        JITDUMP("Not guessing for method; likelihood is below %s call threshold %u\n",
                call->IsDelegateInvoke() ? "delegate" : "virtual", likelihoodThreshold);
    }
}

//------------------------------------------------------------------------
// isCompatibleMethodGDV:
//    Check if devirtualizing a call node as a specified target method call is
//    reasonable.
//
// Arguments:
//    call - the call
//    gdvTarget - the target method that we want to guess for and devirtualize to
//
// Returns:
//    true if we can proceed with GDV.
//
// Notes:
//    This implements a small simplified signature-compatibility check to
//    verify that a guess is reasonable. The main goal here is to avoid blowing
//    up the JIT on PGO data with stale GDV candidates; if they are not
//    compatible in the ECMA sense then we do not expect the guard to ever pass
//    at runtime, so we can get by with simplified rules here.
//
bool Compiler::isCompatibleMethodGDV(GenTreeCall* call, CORINFO_METHOD_HANDLE gdvTarget)
{
    CORINFO_SIG_INFO sig;
    info.compCompHnd->getMethodSig(gdvTarget, &sig);

    CORINFO_ARG_LIST_HANDLE sigParam  = sig.args;
    unsigned                numParams = sig.numArgs;
    unsigned                numArgs   = 0;
    for (CallArg& arg : call->gtArgs.Args())
    {
        switch (arg.GetWellKnownArg())
        {
            case WellKnownArg::RetBuffer:
            case WellKnownArg::ThisPointer:
                // Not part of signature but we still expect to see it here
                continue;
            case WellKnownArg::None:
                break;
            default:
                assert(!"Unexpected well known arg to method GDV candidate");
                continue;
        }

        numArgs++;
        if (numArgs > numParams)
        {
            JITDUMP("Incompatible method GDV: call [%06u] has more arguments than signature (sig has %d parameters)\n",
                    dspTreeID(call), numParams);
            return false;
        }

        CORINFO_CLASS_HANDLE classHnd = NO_CLASS_HANDLE;
        CorInfoType          corType  = strip(info.compCompHnd->getArgType(&sig, sigParam, &classHnd));
        var_types            sigType  = JITtype2varType(corType);

        if (!impCheckImplicitArgumentCoercion(sigType, arg.GetNode()->TypeGet()))
        {
            JITDUMP("Incompatible method GDV: arg [%06u] is type-incompatible with signature of target\n",
                    dspTreeID(arg.GetNode()));
            return false;
        }

        // Best-effort check for struct compatibility here.
        if (varTypeIsStruct(sigType) && (arg.GetSignatureClassHandle() != classHnd))
        {
            ClassLayout* callLayout = typGetObjLayout(arg.GetSignatureClassHandle());
            ClassLayout* tarLayout  = typGetObjLayout(classHnd);

            if (!ClassLayout::AreCompatible(callLayout, tarLayout))
            {
                JITDUMP("Incompatible method GDV: struct arg [%06u] is layout-incompatible with signature of target\n",
                        dspTreeID(arg.GetNode()));
                return false;
            }
        }

        sigParam = info.compCompHnd->getArgNext(sigParam);
    }

    if (numArgs < numParams)
    {
        JITDUMP("Incompatible method GDV: call [%06u] has fewer arguments (%d) than signature (%d)\n", dspTreeID(call),
                numArgs, numParams);
        return false;
    }

    return true;
}

//------------------------------------------------------------------------
// considerGuardedDevirtualization: see if we can profitably guess at the
//    class involved in an interface or virtual call.
//
// Arguments:
//
//    call - potential guarded devirtualization candidate
//    ilOffset - IL ofset of the call instruction
//    baseMethod - target method of the call
//    baseClass - class that introduced the target method
//    pContextHandle - context handle for the call
//
// Notes:
//    Consults with VM to see if there's a likely class at runtime,
//    if so, adds a candidate for guarded devirtualization.
//
void Compiler::considerGuardedDevirtualization(GenTreeCall*            call,
                                               IL_OFFSET               ilOffset,
                                               bool                    isInterface,
                                               CORINFO_METHOD_HANDLE   baseMethod,
                                               CORINFO_CLASS_HANDLE    baseClass,
                                               CORINFO_CONTEXT_HANDLE* pContextHandle)
{
    JITDUMP("Considering guarded devirtualization at IL offset %u (0x%x)\n", ilOffset, ilOffset);

    // We currently only get likely class guesses when there is PGO data
    // with class profiles.
    //
    if ((fgPgoClassProfiles == 0) && (fgPgoMethodProfiles == 0))
    {
        JITDUMP("Not guessing for class or method: no GDV profile pgo data, or pgo disabled\n");
        return;
    }

    CORINFO_CLASS_HANDLE  likelyClass;
    CORINFO_METHOD_HANDLE likelyMethod;
    unsigned              likelihood;
    pickGDV(call, ilOffset, isInterface, &likelyClass, &likelyMethod, &likelihood);

    if ((likelyClass == NO_CLASS_HANDLE) && (likelyMethod == NO_METHOD_HANDLE))
    {
        return;
    }

    uint32_t likelyClassAttribs = 0;
    if (likelyClass != NO_CLASS_HANDLE)
    {
        likelyClassAttribs = info.compCompHnd->getClassAttribs(likelyClass);

        if ((likelyClassAttribs & CORINFO_FLG_ABSTRACT) != 0)
        {
            // We may see an abstract likely class, if we have a stale profile.
            // No point guessing for this.
            //
            JITDUMP("Not guessing for class; abstract (stale profile)\n");
            return;
        }

        // Figure out which method will be called.
        //
        CORINFO_DEVIRTUALIZATION_INFO dvInfo;
        dvInfo.virtualMethod               = baseMethod;
        dvInfo.objClass                    = likelyClass;
        dvInfo.context                     = *pContextHandle;
        dvInfo.exactContext                = *pContextHandle;
        dvInfo.pResolvedTokenVirtualMethod = nullptr;

        const bool canResolve = info.compCompHnd->resolveVirtualMethod(&dvInfo);

        if (!canResolve)
        {
            JITDUMP("Can't figure out which method would be invoked, sorry\n");
            return;
        }

        likelyMethod = dvInfo.devirtualizedMethod;
    }

    uint32_t likelyMethodAttribs = info.compCompHnd->getMethodAttribs(likelyMethod);

    if (likelyClass == NO_CLASS_HANDLE)
    {
        // For method GDV do a few more checks that we get for free in the
        // resolve call above for class-based GDV.
        if ((likelyMethodAttribs & CORINFO_FLG_STATIC) != 0)
        {
            assert((fgPgoSource != ICorJitInfo::PgoSource::Dynamic) || call->IsDelegateInvoke());
            JITDUMP("Cannot currently handle devirtualizing static delegate calls, sorry\n");
            return;
        }

        CORINFO_CLASS_HANDLE definingClass = info.compCompHnd->getMethodClass(likelyMethod);
        likelyClassAttribs                 = info.compCompHnd->getClassAttribs(definingClass);

        // For instance methods on value classes we need an extended check to
        // check for the unboxing stub. This is NYI.
        // Note: For dynamic PGO likelyMethod above will be the unboxing stub
        // which would fail GDV for other reasons.
        // However, with static profiles or textual PGO input it is still
        // possible that likelyMethod is not the unboxing stub. So we do need
        // this explicit check.
        if ((likelyClassAttribs & CORINFO_FLG_VALUECLASS) != 0)
        {
            JITDUMP("Cannot currently handle devirtualizing delegate calls on value types, sorry\n");
            return;
        }

        // Verify that the call target and args look reasonable so that the JIT
        // does not blow up during inlining/call morphing.
        //
        // NOTE: Once we want to support devirtualization of delegate calls to
        // static methods and remove the check above we will start failing here
        // for delegates pointing to static methods that have the first arg
        // bound. For example:
        //
        // public static void E(this C c) ...
        // Action a = new C().E;
        //
        // The delegate instance looks exactly like one pointing to an instance
        // method in this case and the call will have zero args while the
        // signature has 1 arg.
        //
        if (!isCompatibleMethodGDV(call, likelyMethod))
        {
            JITDUMP("Target for method-based GDV is incompatible (stale profile?)\n");
            assert((fgPgoSource != ICorJitInfo::PgoSource::Dynamic) && "Unexpected stale profile in dynamic PGO data");
            return;
        }
    }

    JITDUMP("%s call would invoke method %s\n",
            isInterface ? "interface" : call->IsDelegateInvoke() ? "delegate" : "virtual",
            eeGetMethodName(likelyMethod, nullptr));

    // Add this as a potential candidate.
    //
    addGuardedDevirtualizationCandidate(call, likelyMethod, likelyClass, likelyMethodAttribs, likelyClassAttribs,
                                        likelihood);
}

//------------------------------------------------------------------------
// addGuardedDevirtualizationCandidate: potentially mark the call as a guarded
//    devirtualization candidate
//
// Notes:
//
// Call sites in rare or unoptimized code, and calls that require cookies are
// not marked as candidates.
//
// As part of marking the candidate, the code spills GT_RET_EXPRs anywhere in any
// child tree, because and we need to clone all these trees when we clone the call
// as part of guarded devirtualization, and these IR nodes can't be cloned.
//
// Arguments:
//    call - potential guarded devirtualization candidate
//    methodHandle - method that will be invoked if the class test succeeds
//    classHandle - class that will be tested for at runtime
//    methodAttr - attributes of the method
//    classAttr - attributes of the class
//    likelihood - odds that this class is the class seen at runtime
//
void Compiler::addGuardedDevirtualizationCandidate(GenTreeCall*          call,
                                                   CORINFO_METHOD_HANDLE methodHandle,
                                                   CORINFO_CLASS_HANDLE  classHandle,
                                                   unsigned              methodAttr,
                                                   unsigned              classAttr,
                                                   unsigned              likelihood)
{
    // This transformation only makes sense for delegate and virtual calls
    assert(call->IsDelegateInvoke() || call->IsVirtual());

    // Only mark calls if the feature is enabled.
    const bool isEnabled = JitConfig.JitEnableGuardedDevirtualization() > 0;

    if (!isEnabled)
    {
        JITDUMP("NOT Marking call [%06u] as guarded devirtualization candidate -- disabled by jit config\n",
                dspTreeID(call));
        return;
    }

    // Bail if not optimizing or the call site is very likely cold
    if (compCurBB->isRunRarely() || opts.OptimizationDisabled())
    {
        JITDUMP("NOT Marking call [%06u] as guarded devirtualization candidate -- rare / dbg / minopts\n",
                dspTreeID(call));
        return;
    }

    // CT_INDIRECT calls may use the cookie, bail if so...
    //
    // If transforming these provides a benefit, we could save this off in the same way
    // we save the stub address below.
    if ((call->gtCallType == CT_INDIRECT) && (call->AsCall()->gtCallCookie != nullptr))
    {
        JITDUMP("NOT Marking call [%06u] as guarded devirtualization candidate -- CT_INDIRECT with cookie\n",
                dspTreeID(call));
        return;
    }

#ifdef DEBUG

    // See if disabled by range
    //
    static ConfigMethodRange JitGuardedDevirtualizationRange;
    JitGuardedDevirtualizationRange.EnsureInit(JitConfig.JitGuardedDevirtualizationRange());
    assert(!JitGuardedDevirtualizationRange.Error());
    if (!JitGuardedDevirtualizationRange.Contains(impInlineRoot()->info.compMethodHash()))
    {
        JITDUMP("NOT Marking call [%06u] as guarded devirtualization candidate -- excluded by "
                "JitGuardedDevirtualizationRange",
                dspTreeID(call));
        return;
    }

#endif

    // We're all set, proceed with candidate creation.
    //
    JITDUMP("Marking call [%06u] as guarded devirtualization candidate; will guess for %s %s\n", dspTreeID(call),
            classHandle != NO_CLASS_HANDLE ? "class" : "method",
            classHandle != NO_CLASS_HANDLE ? eeGetClassName(classHandle) : eeGetMethodFullName(methodHandle));
    setMethodHasGuardedDevirtualization();
    call->SetGuardedDevirtualizationCandidate();

    // Spill off any GT_RET_EXPR subtrees so we can clone the call.
    //
    SpillRetExprHelper helper(this);
    helper.StoreRetExprResultsInArgs(call);

    // Gather some information for later. Note we actually allocate InlineCandidateInfo
    // here, as the devirtualized half of this call will likely become an inline candidate.
    //
    GuardedDevirtualizationCandidateInfo* pInfo = new (this, CMK_Inlining) InlineCandidateInfo;

    pInfo->guardedMethodHandle             = methodHandle;
    pInfo->guardedMethodUnboxedEntryHandle = nullptr;
    pInfo->guardedClassHandle              = classHandle;
    pInfo->likelihood                      = likelihood;
    pInfo->requiresInstMethodTableArg      = false;

    // If the guarded class is a value class, look for an unboxed entry point.
    //
    if ((classAttr & CORINFO_FLG_VALUECLASS) != 0)
    {
        JITDUMP("    ... class is a value class, looking for unboxed entry\n");
        bool                  requiresInstMethodTableArg = false;
        CORINFO_METHOD_HANDLE unboxedEntryMethodHandle =
            info.compCompHnd->getUnboxedEntry(methodHandle, &requiresInstMethodTableArg);

        if (unboxedEntryMethodHandle != nullptr)
        {
            JITDUMP("    ... updating GDV candidate with unboxed entry info\n");
            pInfo->guardedMethodUnboxedEntryHandle = unboxedEntryMethodHandle;
            pInfo->requiresInstMethodTableArg      = requiresInstMethodTableArg;
        }
    }

    call->gtGuardedDevirtualizationCandidateInfo = pInfo;
}

//------------------------------------------------------------------------
// impMarkInlineCandidate: determine if this call can be subsequently inlined
//
// Arguments:
//    callNode -- call under scrutiny
//    exactContextHnd -- context handle for inlining
//    exactContextNeedsRuntimeLookup -- true if context required runtime lookup
//    callInfo -- call info from VM
//    ilOffset -- the actual IL offset of the instruction that produced this inline candidate
//
// Notes:
//    Mostly a wrapper for impMarkInlineCandidateHelper that also undoes
//    guarded devirtualization for virtual calls where the method we'd
//    devirtualize to cannot be inlined.

void Compiler::impMarkInlineCandidate(GenTree*               callNode,
                                      CORINFO_CONTEXT_HANDLE exactContextHnd,
                                      bool                   exactContextNeedsRuntimeLookup,
                                      CORINFO_CALL_INFO*     callInfo,
                                      IL_OFFSET              ilOffset)
{
    GenTreeCall* call = callNode->AsCall();

    // Do the actual evaluation
    impMarkInlineCandidateHelper(call, exactContextHnd, exactContextNeedsRuntimeLookup, callInfo, ilOffset);

    // If this call is an inline candidate or is not a guarded devirtualization
    // candidate, we're done.
    if (call->IsInlineCandidate() || !call->IsGuardedDevirtualizationCandidate())
    {
        return;
    }

    // If we can't inline the call we'd guardedly devirtualize to,
    // we undo the guarded devirtualization, as the benefit from
    // just guarded devirtualization alone is likely not worth the
    // extra jit time and code size.
    //
    // TODO: it is possibly interesting to allow this, but requires
    // fixes elsewhere too...
    JITDUMP("Revoking guarded devirtualization candidacy for call [%06u]: target method can't be inlined\n",
            dspTreeID(call));

    call->ClearGuardedDevirtualizationCandidate();
}

//------------------------------------------------------------------------
// impMarkInlineCandidateHelper: determine if this call can be subsequently
//     inlined
//
// Arguments:
//    callNode -- call under scrutiny
//    exactContextHnd -- context handle for inlining
//    exactContextNeedsRuntimeLookup -- true if context required runtime lookup
//    callInfo -- call info from VM
//    ilOffset -- IL offset of instruction creating the inline candidate
//
// Notes:
//    If callNode is an inline candidate, this method sets the flag
//    GTF_CALL_INLINE_CANDIDATE, and ensures that helper methods have
//    filled in the associated InlineCandidateInfo.
//
//    If callNode is not an inline candidate, and the reason is
//    something that is inherent to the method being called, the
//    method may be marked as "noinline" to short-circuit any
//    future assessments of calls to this method.

void Compiler::impMarkInlineCandidateHelper(GenTreeCall*           call,
                                            CORINFO_CONTEXT_HANDLE exactContextHnd,
                                            bool                   exactContextNeedsRuntimeLookup,
                                            CORINFO_CALL_INFO*     callInfo,
                                            IL_OFFSET              ilOffset)
{
    // Let the strategy know there's another call
    impInlineRoot()->m_inlineStrategy->NoteCall();

    if (!opts.OptEnabled(CLFLG_INLINING))
    {
        /* XXX Mon 8/18/2008
         * This assert is misleading.  The caller does not ensure that we have CLFLG_INLINING set before
         * calling impMarkInlineCandidate.  However, if this assert trips it means that we're an inlinee and
         * CLFLG_MINOPT is set.  That doesn't make a lot of sense.  If you hit this assert, work back and
         * figure out why we did not set MAXOPT for this compile.
         */
        assert(!compIsForInlining());
        return;
    }

    if (compIsForImportOnly())
    {
        // Don't bother creating the inline candidate during verification.
        // Otherwise the call to info.compCompHnd->canInline will trigger a recursive verification
        // that leads to the creation of multiple instances of Compiler.
        return;
    }

    InlineResult inlineResult(this, call, nullptr, "impMarkInlineCandidate");

    // Don't inline if not optimizing root method
    if (opts.compDbgCode)
    {
        inlineResult.NoteFatal(InlineObservation::CALLER_DEBUG_CODEGEN);
        return;
    }

    // Don't inline if inlining into this method is disabled.
    if (impInlineRoot()->m_inlineStrategy->IsInliningDisabled())
    {
        inlineResult.NoteFatal(InlineObservation::CALLER_IS_JIT_NOINLINE);
        return;
    }

    // Don't inline into callers that use the NextCallReturnAddress intrinsic.
    if (info.compHasNextCallRetAddr)
    {
        inlineResult.NoteFatal(InlineObservation::CALLER_USES_NEXT_CALL_RET_ADDR);
        return;
    }

    // Inlining candidate determination needs to honor only IL tail prefix.
    // Inlining takes precedence over implicit tail call optimization (if the call is not directly recursive).
    if (call->IsTailPrefixedCall())
    {
        inlineResult.NoteFatal(InlineObservation::CALLSITE_EXPLICIT_TAIL_PREFIX);
        return;
    }

    // Delegate Invoke method doesn't have a body and gets special cased instead.
    // Don't even bother trying to inline it.
    if (call->IsDelegateInvoke() && !call->IsGuardedDevirtualizationCandidate())
    {
        inlineResult.NoteFatal(InlineObservation::CALLEE_HAS_NO_BODY);
        return;
    }

    // Tail recursion elimination takes precedence over inlining.
    // TODO: We may want to do some of the additional checks from fgMorphCall
    // here to reduce the chance we don't inline a call that won't be optimized
    // as a fast tail call or turned into a loop.
    if (gtIsRecursiveCall(call) && call->IsImplicitTailCall())
    {
        inlineResult.NoteFatal(InlineObservation::CALLSITE_IMPLICIT_REC_TAIL_CALL);
        return;
    }

    if (call->IsVirtual())
    {
        // Allow guarded devirt calls to be treated as inline candidates,
        // but reject all other virtual calls.
        if (!call->IsGuardedDevirtualizationCandidate())
        {
            inlineResult.NoteFatal(InlineObservation::CALLSITE_IS_NOT_DIRECT);
            return;
        }
    }

    /* Ignore helper calls */

    if (call->gtCallType == CT_HELPER)
    {
        assert(!call->IsGuardedDevirtualizationCandidate());
        inlineResult.NoteFatal(InlineObservation::CALLSITE_IS_CALL_TO_HELPER);
        return;
    }

    /* Ignore indirect calls */
    if (call->gtCallType == CT_INDIRECT)
    {
        inlineResult.NoteFatal(InlineObservation::CALLSITE_IS_NOT_DIRECT_MANAGED);
        return;
    }

    /* I removed the check for BBJ_THROW.  BBJ_THROW is usually marked as rarely run.  This more or less
     * restricts the inliner to non-expanding inlines.  I removed the check to allow for non-expanding
     * inlining in throw blocks.  I should consider the same thing for catch and filter regions. */

    CORINFO_METHOD_HANDLE fncHandle;
    unsigned              methAttr;

    if (call->IsGuardedDevirtualizationCandidate())
    {
        if (call->gtGuardedDevirtualizationCandidateInfo->guardedMethodUnboxedEntryHandle != nullptr)
        {
            fncHandle = call->gtGuardedDevirtualizationCandidateInfo->guardedMethodUnboxedEntryHandle;
        }
        else
        {
            fncHandle = call->gtGuardedDevirtualizationCandidateInfo->guardedMethodHandle;
        }
        methAttr = info.compCompHnd->getMethodAttribs(fncHandle);
    }
    else
    {
        fncHandle = call->gtCallMethHnd;

        // Reuse method flags from the original callInfo if possible
        if (fncHandle == callInfo->hMethod)
        {
            methAttr = callInfo->methodFlags;
        }
        else
        {
            methAttr = info.compCompHnd->getMethodAttribs(fncHandle);
        }
    }

#ifdef DEBUG
    if (compStressCompile(STRESS_FORCE_INLINE, 0))
    {
        methAttr |= CORINFO_FLG_FORCEINLINE;
    }
#endif

    // Check for COMPlus_AggressiveInlining
    if (compDoAggressiveInlining)
    {
        methAttr |= CORINFO_FLG_FORCEINLINE;
    }

    if (!(methAttr & CORINFO_FLG_FORCEINLINE))
    {
        /* Don't bother inline blocks that are in the filter region */
        if (bbInCatchHandlerILRange(compCurBB))
        {
#ifdef DEBUG
            if (verbose)
            {
                printf("\nWill not inline blocks that are in the catch handler region\n");
            }

#endif

            inlineResult.NoteFatal(InlineObservation::CALLSITE_IS_WITHIN_CATCH);
            return;
        }

        if (bbInFilterILRange(compCurBB))
        {
#ifdef DEBUG
            if (verbose)
            {
                printf("\nWill not inline blocks that are in the filter region\n");
            }
#endif

            inlineResult.NoteFatal(InlineObservation::CALLSITE_IS_WITHIN_FILTER);
            return;
        }
    }

    /* Check if we tried to inline this method before */

    if (methAttr & CORINFO_FLG_DONT_INLINE)
    {
        inlineResult.NoteFatal(InlineObservation::CALLEE_IS_NOINLINE);
        return;
    }

    /* Cannot inline synchronized methods */

    if (methAttr & CORINFO_FLG_SYNCH)
    {
        inlineResult.NoteFatal(InlineObservation::CALLEE_IS_SYNCHRONIZED);
        return;
    }

    /* Check legality of PInvoke callsite (for inlining of marshalling code) */

    if (methAttr & CORINFO_FLG_PINVOKE)
    {
        // See comment in impCheckForPInvokeCall
        BasicBlock* block = compIsForInlining() ? impInlineInfo->iciBlock : compCurBB;
        if (!impCanPInvokeInlineCallSite(block))
        {
            inlineResult.NoteFatal(InlineObservation::CALLSITE_PINVOKE_EH);
            return;
        }
    }

    InlineCandidateInfo* inlineCandidateInfo = nullptr;
    impCheckCanInline(call, fncHandle, methAttr, exactContextHnd, &inlineCandidateInfo, &inlineResult);

    if (inlineResult.IsFailure())
    {
        return;
    }

    // The old value should be null OR this call should be a guarded devirtualization candidate.
    assert((call->gtInlineCandidateInfo == nullptr) || call->IsGuardedDevirtualizationCandidate());

    // The new value should not be null.
    assert(inlineCandidateInfo != nullptr);
    inlineCandidateInfo->exactContextNeedsRuntimeLookup = exactContextNeedsRuntimeLookup;
    inlineCandidateInfo->ilOffset                       = ilOffset;
    call->gtInlineCandidateInfo                         = inlineCandidateInfo;

    // If we're in an inlinee compiler, and have a return spill temp, and this inline candidate
    // is also a tail call candidate, it can use the same return spill temp.
    //
    if (compIsForInlining() && call->CanTailCall() &&
        (impInlineInfo->inlineCandidateInfo->preexistingSpillTemp != BAD_VAR_NUM))
    {
        inlineCandidateInfo->preexistingSpillTemp = impInlineInfo->inlineCandidateInfo->preexistingSpillTemp;
        JITDUMP("Inline candidate [%06u] can share spill temp V%02u\n", dspTreeID(call),
                inlineCandidateInfo->preexistingSpillTemp);
    }

    // Mark the call node as inline candidate.
    call->gtFlags |= GTF_CALL_INLINE_CANDIDATE;

    // Let the strategy know there's another candidate.
    impInlineRoot()->m_inlineStrategy->NoteCandidate();

    // Since we're not actually inlining yet, and this call site is
    // still just an inline candidate, there's nothing to report.
    inlineResult.SetSuccessResult(INLINE_CHECK_CAN_INLINE_SUCCESS);
}

/******************************************************************************/
// Returns true if the given intrinsic will be implemented by target-specific
// instructions

bool Compiler::IsTargetIntrinsic(NamedIntrinsic intrinsicName)
{
#if defined(TARGET_XARCH)
    switch (intrinsicName)
    {
        // AMD64/x86 has SSE2 instructions to directly compute sqrt/abs and SSE4.1
        // instructions to directly compute round/ceiling/floor/truncate.

        case NI_System_Math_Abs:
        case NI_System_Math_Sqrt:
            return true;

        case NI_System_Math_Ceiling:
        case NI_System_Math_Floor:
        case NI_System_Math_Truncate:
        case NI_System_Math_Round:
            return compOpportunisticallyDependsOn(InstructionSet_SSE41);

        case NI_System_Math_FusedMultiplyAdd:
            return compOpportunisticallyDependsOn(InstructionSet_FMA);

        default:
            return false;
    }
#elif defined(TARGET_ARM64)
    switch (intrinsicName)
    {
        case NI_System_Math_Abs:
        case NI_System_Math_Ceiling:
        case NI_System_Math_Floor:
        case NI_System_Math_Truncate:
        case NI_System_Math_Round:
        case NI_System_Math_Sqrt:
        case NI_System_Math_Max:
        case NI_System_Math_Min:
            return true;

        case NI_System_Math_FusedMultiplyAdd:
            return compOpportunisticallyDependsOn(InstructionSet_AdvSimd);

        default:
            return false;
    }
#elif defined(TARGET_ARM)
    switch (intrinsicName)
    {
        case NI_System_Math_Abs:
        case NI_System_Math_Round:
        case NI_System_Math_Sqrt:
            return true;

        default:
            return false;
    }
#elif defined(TARGET_LOONGARCH64)
    // TODO-LoongArch64: add some intrinsics.
    return false;
#else
    // TODO: This portion of logic is not implemented for other arch.
    // The reason for returning true is that on all other arch the only intrinsic
    // enabled are target intrinsics.
    return true;
#endif
}

/******************************************************************************/
// Returns true if the given intrinsic will be implemented by calling System.Math
// methods.

bool Compiler::IsIntrinsicImplementedByUserCall(NamedIntrinsic intrinsicName)
{
    // Currently, if a math intrinsic is not implemented by target-specific
    // instructions, it will be implemented by a System.Math call. In the
    // future, if we turn to implementing some of them with helper calls,
    // this predicate needs to be revisited.
    return !IsTargetIntrinsic(intrinsicName);
}

bool Compiler::IsMathIntrinsic(NamedIntrinsic intrinsicName)
{
    switch (intrinsicName)
    {
        case NI_System_Math_Abs:
        case NI_System_Math_Acos:
        case NI_System_Math_Acosh:
        case NI_System_Math_Asin:
        case NI_System_Math_Asinh:
        case NI_System_Math_Atan:
        case NI_System_Math_Atanh:
        case NI_System_Math_Atan2:
        case NI_System_Math_Cbrt:
        case NI_System_Math_Ceiling:
        case NI_System_Math_Cos:
        case NI_System_Math_Cosh:
        case NI_System_Math_Exp:
        case NI_System_Math_Floor:
        case NI_System_Math_FMod:
        case NI_System_Math_FusedMultiplyAdd:
        case NI_System_Math_ILogB:
        case NI_System_Math_Log:
        case NI_System_Math_Log2:
        case NI_System_Math_Log10:
        case NI_System_Math_Max:
        case NI_System_Math_Min:
        case NI_System_Math_Pow:
        case NI_System_Math_Round:
        case NI_System_Math_Sin:
        case NI_System_Math_Sinh:
        case NI_System_Math_Sqrt:
        case NI_System_Math_Tan:
        case NI_System_Math_Tanh:
        case NI_System_Math_Truncate:
        {
            assert((intrinsicName > NI_SYSTEM_MATH_START) && (intrinsicName < NI_SYSTEM_MATH_END));
            return true;
        }

        default:
        {
            assert((intrinsicName < NI_SYSTEM_MATH_START) || (intrinsicName > NI_SYSTEM_MATH_END));
            return false;
        }
    }
}

bool Compiler::IsMathIntrinsic(GenTree* tree)
{
    return (tree->OperGet() == GT_INTRINSIC) && IsMathIntrinsic(tree->AsIntrinsic()->gtIntrinsicName);
}

//------------------------------------------------------------------------
// impDevirtualizeCall: Attempt to change a virtual vtable call into a
//   normal call
//
// Arguments:
//     call -- the call node to examine/modify
//     pResolvedToken -- [IN] the resolved token used to create the call. Used for R2R.
//     method   -- [IN/OUT] the method handle for call. Updated iff call devirtualized.
//     methodFlags -- [IN/OUT] flags for the method to call. Updated iff call devirtualized.
//     pContextHandle -- [IN/OUT] context handle for the call. Updated iff call devirtualized.
//     pExactContextHandle -- [OUT] updated context handle iff call devirtualized
//     isLateDevirtualization -- if devirtualization is happening after importation
//     isExplicitTailCalll -- [IN] true if we plan on using an explicit tail call
//     ilOffset -- IL offset of the call
//
// Notes:
//     Virtual calls in IL will always "invoke" the base class method.
//
//     This transformation looks for evidence that the type of 'this'
//     in the call is exactly known, is a final class or would invoke
//     a final method, and if that and other safety checks pan out,
//     modifies the call and the call info to create a direct call.
//
//     This transformation is initially done in the importer and not
//     in some subsequent optimization pass because we want it to be
//     upstream of inline candidate identification.
//
//     However, later phases may supply improved type information that
//     can enable further devirtualization. We currently reinvoke this
//     code after inlining, if the return value of the inlined call is
//     the 'this obj' of a subsequent virtual call.
//
//     If devirtualization succeeds and the call's this object is a
//     (boxed) value type, the jit will ask the EE for the unboxed entry
//     point. If this exists, the jit will invoke the unboxed entry
//     on the box payload. In addition if the boxing operation is
//     visible to the jit and the call is the only consmer of the box,
//     the jit will try analyze the box to see if the call can be instead
//     instead made on a local copy. If that is doable, the call is
//     updated to invoke the unboxed entry on the local copy and the
//     boxing operation is removed.
//
//     When guarded devirtualization is enabled, this method will mark
//     calls as guarded devirtualization candidates, if the type of `this`
//     is not exactly known, and there is a plausible guess for the type.
void Compiler::impDevirtualizeCall(GenTreeCall*            call,
                                   CORINFO_RESOLVED_TOKEN* pResolvedToken,
                                   CORINFO_METHOD_HANDLE*  method,
                                   unsigned*               methodFlags,
                                   CORINFO_CONTEXT_HANDLE* pContextHandle,
                                   CORINFO_CONTEXT_HANDLE* pExactContextHandle,
                                   bool                    isLateDevirtualization,
                                   bool                    isExplicitTailCall,
                                   IL_OFFSET               ilOffset)
{
    assert(call != nullptr);
    assert(method != nullptr);
    assert(methodFlags != nullptr);
    assert(pContextHandle != nullptr);

    // This should be a virtual vtable or virtual stub call.
    //
    assert(call->IsVirtual());
    assert(opts.OptimizationEnabled());

#if defined(DEBUG)
    // Bail if devirt is disabled.
    if (JitConfig.JitEnableDevirtualization() == 0)
    {
        return;
    }

    // Optionally, print info on devirtualization
    Compiler* const rootCompiler = impInlineRoot();
    const bool      doPrint      = JitConfig.JitPrintDevirtualizedMethods().contains(rootCompiler->info.compMethodHnd,
                                                                           rootCompiler->info.compClassHnd,
                                                                           &rootCompiler->info.compMethodInfo->args);
#endif // DEBUG

    // Fetch information about the virtual method we're calling.
    CORINFO_METHOD_HANDLE baseMethod        = *method;
    unsigned              baseMethodAttribs = *methodFlags;

    if (baseMethodAttribs == 0)
    {
        // For late devirt we may not have method attributes, so fetch them.
        baseMethodAttribs = info.compCompHnd->getMethodAttribs(baseMethod);
    }
    else
    {
#if defined(DEBUG)
        // Validate that callInfo has up to date method flags
        const DWORD freshBaseMethodAttribs = info.compCompHnd->getMethodAttribs(baseMethod);

        // All the base method attributes should agree, save that
        // CORINFO_FLG_DONT_INLINE may have changed from 0 to 1
        // because of concurrent jitting activity.
        //
        // Note we don't look at this particular flag bit below, and
        // later on (if we do try and inline) we will rediscover why
        // the method can't be inlined, so there's no danger here in
        // seeing this particular flag bit in different states between
        // the cached and fresh values.
        if ((freshBaseMethodAttribs & ~CORINFO_FLG_DONT_INLINE) != (baseMethodAttribs & ~CORINFO_FLG_DONT_INLINE))
        {
            assert(!"mismatched method attributes");
        }
#endif // DEBUG
    }

    // In R2R mode, we might see virtual stub calls to
    // non-virtuals. For instance cases where the non-virtual method
    // is in a different assembly but is called via CALLVIRT. For
    // version resilience we must allow for the fact that the method
    // might become virtual in some update.
    //
    // In non-R2R modes CALLVIRT <nonvirtual> will be turned into a
    // regular call+nullcheck upstream, so we won't reach this
    // point.
    if ((baseMethodAttribs & CORINFO_FLG_VIRTUAL) == 0)
    {
        assert(call->IsVirtualStub());
        assert(opts.IsReadyToRun());
        JITDUMP("\nimpDevirtualizeCall: [R2R] base method not virtual, sorry\n");
        return;
    }

    // Fetch information about the class that introduced the virtual method.
    CORINFO_CLASS_HANDLE baseClass        = info.compCompHnd->getMethodClass(baseMethod);
    const DWORD          baseClassAttribs = info.compCompHnd->getClassAttribs(baseClass);

    // Is the call an interface call?
    const bool isInterface = (baseClassAttribs & CORINFO_FLG_INTERFACE) != 0;

    // See what we know about the type of 'this' in the call.
    assert(call->gtArgs.HasThisPointer());
    CallArg*             thisArg      = call->gtArgs.GetThisArg();
    GenTree*             thisObj      = thisArg->GetEarlyNode()->gtEffectiveVal(false);
    bool                 isExact      = false;
    bool                 objIsNonNull = false;
    CORINFO_CLASS_HANDLE objClass     = gtGetClassHandle(thisObj, &isExact, &objIsNonNull);

    // Bail if we know nothing.
    if (objClass == NO_CLASS_HANDLE)
    {
        JITDUMP("\nimpDevirtualizeCall: no type available (op=%s)\n", GenTree::OpName(thisObj->OperGet()));

        // Don't try guarded devirtualiztion when we're doing late devirtualization.
        //
        if (isLateDevirtualization)
        {
            JITDUMP("No guarded devirt during late devirtualization\n");
            return;
        }

        considerGuardedDevirtualization(call, ilOffset, isInterface, baseMethod, baseClass, pContextHandle);

        return;
    }

    // If the objClass is sealed (final), then we may be able to devirtualize.
    const DWORD objClassAttribs = info.compCompHnd->getClassAttribs(objClass);
    const bool  objClassIsFinal = (objClassAttribs & CORINFO_FLG_FINAL) != 0;

#if defined(DEBUG)
    const char* callKind       = isInterface ? "interface" : "virtual";
    const char* objClassNote   = "[?]";
    const char* objClassName   = "?objClass";
    const char* baseClassName  = "?baseClass";
    const char* baseMethodName = "?baseMethod";

    if (verbose || doPrint)
    {
        objClassNote   = isExact ? " [exact]" : objClassIsFinal ? " [final]" : "";
        objClassName   = eeGetClassName(objClass);
        baseClassName  = eeGetClassName(baseClass);
        baseMethodName = eeGetMethodName(baseMethod, nullptr);

        if (verbose)
        {
            printf("\nimpDevirtualizeCall: Trying to devirtualize %s call:\n"
                   "    class for 'this' is %s%s (attrib %08x)\n"
                   "    base method is %s::%s\n",
                   callKind, objClassName, objClassNote, objClassAttribs, baseClassName, baseMethodName);
        }
    }
#endif // defined(DEBUG)

    // See if the jit's best type for `obj` is an interface.
    // See for instance System.ValueTuple`8::GetHashCode, where lcl 0 is System.IValueTupleInternal
    //   IL_021d:  ldloc.0
    //   IL_021e:  callvirt   instance int32 System.Object::GetHashCode()
    //
    // If so, we can't devirtualize, but we may be able to do guarded devirtualization.
    //
    if ((objClassAttribs & CORINFO_FLG_INTERFACE) != 0)
    {
        // Don't try guarded devirtualiztion when we're doing late devirtualization.
        //
        if (isLateDevirtualization)
        {
            JITDUMP("No guarded devirt during late devirtualization\n");
            return;
        }

        considerGuardedDevirtualization(call, ilOffset, isInterface, baseMethod, baseClass, pContextHandle);
        return;
    }

    // If we get this far, the jit has a lower bound class type for the `this` object being used for dispatch.
    // It may or may not know enough to devirtualize...
    if (isInterface)
    {
        assert(call->IsVirtualStub());
        JITDUMP("--- base class is interface\n");
    }

    // Fetch the method that would be called based on the declared type of 'this',
    // and prepare to fetch the method attributes.
    //
    CORINFO_DEVIRTUALIZATION_INFO dvInfo;
    dvInfo.virtualMethod               = baseMethod;
    dvInfo.objClass                    = objClass;
    dvInfo.context                     = *pContextHandle;
    dvInfo.detail                      = CORINFO_DEVIRTUALIZATION_UNKNOWN;
    dvInfo.pResolvedTokenVirtualMethod = pResolvedToken;

    info.compCompHnd->resolveVirtualMethod(&dvInfo);

    CORINFO_METHOD_HANDLE   derivedMethod         = dvInfo.devirtualizedMethod;
    CORINFO_CONTEXT_HANDLE  exactContext          = dvInfo.exactContext;
    CORINFO_CLASS_HANDLE    derivedClass          = NO_CLASS_HANDLE;
    CORINFO_RESOLVED_TOKEN* pDerivedResolvedToken = &dvInfo.resolvedTokenDevirtualizedMethod;

    if (derivedMethod != nullptr)
    {
        assert(exactContext != nullptr);
        assert(((size_t)exactContext & CORINFO_CONTEXTFLAGS_MASK) == CORINFO_CONTEXTFLAGS_CLASS);
        derivedClass = (CORINFO_CLASS_HANDLE)((size_t)exactContext & ~CORINFO_CONTEXTFLAGS_MASK);
    }

    DWORD derivedMethodAttribs = 0;
    bool  derivedMethodIsFinal = false;
    bool  canDevirtualize      = false;

#if defined(DEBUG)
    const char* derivedClassName  = "?derivedClass";
    const char* derivedMethodName = "?derivedMethod";
    const char* note              = "inexact or not final";
#endif

    // If we failed to get a method handle, we can't directly devirtualize.
    //
    // This can happen when prejitting, if the devirtualization crosses
    // servicing bubble boundaries, or if objClass is a shared class.
    //
    if (derivedMethod == nullptr)
    {
        JITDUMP("--- no derived method: %s\n", devirtualizationDetailToString(dvInfo.detail));
    }
    else
    {
        // Fetch method attributes to see if method is marked final.
        derivedMethodAttribs = info.compCompHnd->getMethodAttribs(derivedMethod);
        derivedMethodIsFinal = ((derivedMethodAttribs & CORINFO_FLG_FINAL) != 0);

#if defined(DEBUG)
        if (isExact)
        {
            note = "exact";
        }
        else if (objClassIsFinal)
        {
            note = "final class";
        }
        else if (derivedMethodIsFinal)
        {
            note = "final method";
        }

        if (verbose || doPrint)
        {
            derivedMethodName = eeGetMethodName(derivedMethod, nullptr);
            derivedClassName  = eeGetClassName(derivedClass);
            if (verbose)
            {
                printf("    devirt to %s::%s -- %s\n", derivedClassName, derivedMethodName, note);
                gtDispTree(call);
            }
        }
#endif // defined(DEBUG)

        canDevirtualize = isExact || objClassIsFinal || (!isInterface && derivedMethodIsFinal);
    }

    // We still might be able to do a guarded devirtualization.
    // Note the call might be an interface call or a virtual call.
    //
    if (!canDevirtualize)
    {
        JITDUMP("    Class not final or exact%s\n", isInterface ? "" : ", and method not final");

#if defined(DEBUG)
        // If we know the object type exactly, we generally expect we can devirtualize.
        // (don't when doing late devirt as we won't have an owner type (yet))
        //
        if (!isLateDevirtualization && (isExact || objClassIsFinal) && JitConfig.JitNoteFailedExactDevirtualization())
        {
            printf("@@@ Exact/Final devirt failure in %s at [%06u] $ %s\n", info.compFullName, dspTreeID(call),
                   devirtualizationDetailToString(dvInfo.detail));
        }
#endif

        // Don't try guarded devirtualiztion if we're doing late devirtualization.
        //
        if (isLateDevirtualization)
        {
            JITDUMP("No guarded devirt during late devirtualization\n");
            return;
        }

        considerGuardedDevirtualization(call, ilOffset, isInterface, baseMethod, baseClass, pContextHandle);
        return;
    }

    // All checks done. Time to transform the call.
    //
    // We should always have an exact class context.
    //
    // Note that wouldnt' be true if the runtime side supported array interface devirt,
    // the resulting method would be a generic method of the non-generic SZArrayHelper class.
    //
    assert(canDevirtualize);

    JITDUMP("    %s; can devirtualize\n", note);

    // Make the updates.
    call->gtFlags &= ~GTF_CALL_VIRT_VTABLE;
    call->gtFlags &= ~GTF_CALL_VIRT_STUB;
    call->gtCallMethHnd = derivedMethod;
    call->gtCallType    = CT_USER_FUNC;
    call->gtControlExpr = nullptr;
    call->gtCallMoreFlags |= GTF_CALL_M_DEVIRTUALIZED;

    // Virtual calls include an implicit null check, which we may
    // now need to make explicit.
    if (!objIsNonNull)
    {
        call->gtFlags |= GTF_CALL_NULLCHECK;
    }

    // Clear the inline candidate info (may be non-null since
    // it's a union field used for other things by virtual
    // stubs)
    call->gtInlineCandidateInfo = nullptr;
    call->gtCallMoreFlags &= ~GTF_CALL_M_HAS_LATE_DEVIRT_INFO;

#if defined(DEBUG)
    if (verbose)
    {
        printf("... after devirt...\n");
        gtDispTree(call);
    }

    if (doPrint)
    {
        printf("Devirtualized %s call to %s:%s; now direct call to %s:%s [%s]\n", callKind, baseClassName,
               baseMethodName, derivedClassName, derivedMethodName, note);
    }

    // If we successfully devirtualized based on an exact or final class,
    // and we have dynamic PGO data describing the likely class, make sure they agree.
    //
    // If pgo source is not dynamic we may see likely classes from other versions of this code
    // where types had different properties.
    //
    // If method is an inlinee we may be specializing to a class that wasn't seen at runtime.
    //
    const bool canSensiblyCheck =
        (isExact || objClassIsFinal) && (fgPgoSource == ICorJitInfo::PgoSource::Dynamic) && !compIsForInlining();
    if (JitConfig.JitCrossCheckDevirtualizationAndPGO() && canSensiblyCheck)
    {
        // We only can handle a single likely class for now
        const int               maxLikelyClasses = 1;
        LikelyClassMethodRecord likelyClasses[maxLikelyClasses];

        UINT32 numberOfClasses =
            getLikelyClasses(likelyClasses, maxLikelyClasses, fgPgoSchema, fgPgoSchemaCount, fgPgoData, ilOffset);
        UINT32 likelihood = likelyClasses[0].likelihood;

        CORINFO_CLASS_HANDLE likelyClass = (CORINFO_CLASS_HANDLE)likelyClasses[0].handle;

        if (numberOfClasses > 0)
        {
            // PGO had better agree the class we devirtualized to is plausible.
            //
            if (likelyClass != derivedClass)
            {
                // Managed type system may report different addresses for a class handle
                // at different times....?
                //
                // Also, AOT may have a more nuanced notion of class equality.
                //
                if (!opts.jitFlags->IsSet(JitFlags::JIT_FLAG_PREJIT))
                {
                    bool mismatch = true;

                    // derivedClass will be the introducer of derived method, so it's possible
                    // likelyClass is a non-overriding subclass. Check up the hierarchy.
                    //
                    CORINFO_CLASS_HANDLE parentClass = likelyClass;
                    while (parentClass != NO_CLASS_HANDLE)
                    {
                        if (parentClass == derivedClass)
                        {
                            mismatch = false;
                            break;
                        }

                        parentClass = info.compCompHnd->getParentType(parentClass);
                    }

                    if (mismatch || (numberOfClasses != 1) || (likelihood != 100))
                    {
                        printf("@@@ Likely %p (%s) != Derived %p (%s) [n=%u, l=%u, il=%u] in %s \n", likelyClass,
                               eeGetClassName(likelyClass), derivedClass, eeGetClassName(derivedClass), numberOfClasses,
                               likelihood, ilOffset, info.compFullName);
                    }

                    assert(!(mismatch || (numberOfClasses != 1) || (likelihood != 100)));
                }
            }
        }
    }
#endif // defined(DEBUG)

    // If the 'this' object is a value class, see if we can rework the call to invoke the
    // unboxed entry. This effectively inlines the normally un-inlineable wrapper stub
    // and exposes the potentially inlinable unboxed entry method.
    //
    // We won't optimize explicit tail calls, as ensuring we get the right tail call info
    // is tricky (we'd need to pass an updated sig and resolved token back to some callers).
    //
    // Note we may not have a derived class in some cases (eg interface call on an array)
    //
    if (info.compCompHnd->isValueClass(derivedClass))
    {
        if (isExplicitTailCall)
        {
            JITDUMP("Have a direct explicit tail call to boxed entry point; can't optimize further\n");
        }
        else
        {
            JITDUMP("Have a direct call to boxed entry point. Trying to optimize to call an unboxed entry point\n");

            // Note for some shared methods the unboxed entry point requires an extra parameter.
            bool                  requiresInstMethodTableArg = false;
            CORINFO_METHOD_HANDLE unboxedEntryMethod =
                info.compCompHnd->getUnboxedEntry(derivedMethod, &requiresInstMethodTableArg);

            if (unboxedEntryMethod != nullptr)
            {
                bool optimizedTheBox = false;

                // If the 'this' object is a local box, see if we can revise things
                // to not require boxing.
                //
                if (thisObj->IsBoxedValue() && !isExplicitTailCall)
                {
                    // Since the call is the only consumer of the box, we know the box can't escape
                    // since it is being passed an interior pointer.
                    //
                    // So, revise the box to simply create a local copy, use the address of that copy
                    // as the this pointer, and update the entry point to the unboxed entry.
                    //
                    // Ideally, we then inline the boxed method and and if it turns out not to modify
                    // the copy, we can undo the copy too.
                    GenTree* localCopyThis = nullptr;

                    if (requiresInstMethodTableArg)
                    {
                        // Perform a trial box removal and ask for the type handle tree that fed the box.
                        //
                        JITDUMP("Unboxed entry needs method table arg...\n");
                        GenTree* methodTableArg =
                            gtTryRemoveBoxUpstreamEffects(thisObj, BR_DONT_REMOVE_WANT_TYPE_HANDLE);

                        if (methodTableArg != nullptr)
                        {
                            // If that worked, turn the box into a copy to a local var
                            //
                            JITDUMP("Found suitable method table arg tree [%06u]\n", dspTreeID(methodTableArg));
                            localCopyThis = gtTryRemoveBoxUpstreamEffects(thisObj, BR_MAKE_LOCAL_COPY);

                            if (localCopyThis != nullptr)
                            {
                                // Pass the local var as this and the type handle as a new arg
                                //
                                JITDUMP("Success! invoking unboxed entry point on local copy, and passing method table "
                                        "arg\n");
                                // TODO-CallArgs-REVIEW: Might discard commas otherwise?
                                assert(thisObj == thisArg->GetEarlyNode());
                                thisArg->SetEarlyNode(localCopyThis);
                                call->gtCallMoreFlags |= GTF_CALL_M_UNBOXED;

                                call->gtArgs.InsertInstParam(this, methodTableArg);

                                call->gtCallMethHnd   = unboxedEntryMethod;
                                derivedMethod         = unboxedEntryMethod;
                                pDerivedResolvedToken = &dvInfo.resolvedTokenDevirtualizedUnboxedMethod;

                                // Method attributes will differ because unboxed entry point is shared
                                //
                                const DWORD unboxedMethodAttribs =
                                    info.compCompHnd->getMethodAttribs(unboxedEntryMethod);
                                JITDUMP("Updating method attribs from 0x%08x to 0x%08x\n", derivedMethodAttribs,
                                        unboxedMethodAttribs);
                                derivedMethodAttribs = unboxedMethodAttribs;
                                optimizedTheBox      = true;
                            }
                            else
                            {
                                JITDUMP("Sorry, failed to undo the box -- can't convert to local copy\n");
                            }
                        }
                        else
                        {
                            JITDUMP("Sorry, failed to undo the box -- can't find method table arg\n");
                        }
                    }
                    else
                    {
                        JITDUMP("Found unboxed entry point, trying to simplify box to a local copy\n");
                        localCopyThis = gtTryRemoveBoxUpstreamEffects(thisObj, BR_MAKE_LOCAL_COPY);

                        if (localCopyThis != nullptr)
                        {
                            JITDUMP("Success! invoking unboxed entry point on local copy\n");
                            assert(thisObj == thisArg->GetEarlyNode());
                            // TODO-CallArgs-REVIEW: Might discard commas otherwise?
                            thisArg->SetEarlyNode(localCopyThis);
                            call->gtCallMethHnd = unboxedEntryMethod;
                            call->gtCallMoreFlags |= GTF_CALL_M_UNBOXED;
                            derivedMethod         = unboxedEntryMethod;
                            pDerivedResolvedToken = &dvInfo.resolvedTokenDevirtualizedUnboxedMethod;

                            optimizedTheBox = true;
                        }
                        else
                        {
                            JITDUMP("Sorry, failed to undo the box\n");
                        }
                    }

                    if (optimizedTheBox)
                    {
                        assert(localCopyThis->OperIs(GT_ADDR));

                        // We may end up inlining this call, so the local copy must be marked as "aliased",
                        // making sure the inlinee importer will know when to spill references to its value.
                        lvaGetDesc(localCopyThis->AsUnOp()->gtOp1->AsLclVar())->lvHasLdAddrOp = true;

#if FEATURE_TAILCALL_OPT
                        if (call->IsImplicitTailCall())
                        {
                            JITDUMP("Clearing the implicit tail call flag\n");

                            // If set, we clear the implicit tail call flag
                            // as we just introduced a new address taken local variable
                            //
                            call->gtCallMoreFlags &= ~GTF_CALL_M_IMPLICIT_TAILCALL;
                        }
#endif // FEATURE_TAILCALL_OPT
                    }
                }

                if (!optimizedTheBox)
                {
                    // If we get here, we have a boxed value class that either wasn't boxed
                    // locally, or was boxed locally but we were unable to remove the box for
                    // various reasons.
                    //
                    // We can still update the call to invoke the unboxed entry, if the
                    // boxed value is simple.
                    //
                    if (requiresInstMethodTableArg)
                    {
                        // Get the method table from the boxed object.
                        //
                        // TODO-CallArgs-REVIEW: Use thisObj here? Differs by gtEffectiveVal.
                        GenTree* const clonedThisArg = gtClone(thisArg->GetEarlyNode());

                        if (clonedThisArg == nullptr)
                        {
                            JITDUMP(
                                "unboxed entry needs MT arg, but `this` was too complex to clone. Deferring update.\n");
                        }
                        else
                        {
                            JITDUMP("revising call to invoke unboxed entry with additional method table arg\n");

                            GenTree* const methodTableArg = gtNewMethodTableLookup(clonedThisArg);

                            // Update the 'this' pointer to refer to the box payload
                            //
                            GenTree* const payloadOffset = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
                            GenTree* const boxPayload =
                                gtNewOperNode(GT_ADD, TYP_BYREF, thisArg->GetEarlyNode(), payloadOffset);

                            assert(thisObj == thisArg->GetEarlyNode());
                            thisArg->SetEarlyNode(boxPayload);
                            call->gtCallMethHnd = unboxedEntryMethod;
                            call->gtCallMoreFlags |= GTF_CALL_M_UNBOXED;

                            // Method attributes will differ because unboxed entry point is shared
                            //
                            const DWORD unboxedMethodAttribs = info.compCompHnd->getMethodAttribs(unboxedEntryMethod);
                            JITDUMP("Updating method attribs from 0x%08x to 0x%08x\n", derivedMethodAttribs,
                                    unboxedMethodAttribs);
                            derivedMethod         = unboxedEntryMethod;
                            pDerivedResolvedToken = &dvInfo.resolvedTokenDevirtualizedUnboxedMethod;
                            derivedMethodAttribs  = unboxedMethodAttribs;

                            call->gtArgs.InsertInstParam(this, methodTableArg);
                        }
                    }
                    else
                    {
                        JITDUMP("revising call to invoke unboxed entry\n");

                        GenTree* const payloadOffset = gtNewIconNode(TARGET_POINTER_SIZE, TYP_I_IMPL);
                        GenTree* const boxPayload =
                            gtNewOperNode(GT_ADD, TYP_BYREF, thisArg->GetEarlyNode(), payloadOffset);

                        thisArg->SetEarlyNode(boxPayload);
                        call->gtCallMethHnd = unboxedEntryMethod;
                        call->gtCallMoreFlags |= GTF_CALL_M_UNBOXED;
                        derivedMethod         = unboxedEntryMethod;
                        pDerivedResolvedToken = &dvInfo.resolvedTokenDevirtualizedUnboxedMethod;
                    }
                }
            }
            else
            {
                // Many of the low-level methods on value classes won't have unboxed entries,
                // as they need access to the type of the object.
                //
                // Note this may be a cue for us to stack allocate the boxed object, since
                // we probably know that these objects don't escape.
                JITDUMP("Sorry, failed to find unboxed entry point\n");
            }
        }
    }

    // Need to update call info too.
    //
    *method      = derivedMethod;
    *methodFlags = derivedMethodAttribs;

    // Update context handle
    //
    *pContextHandle = MAKE_METHODCONTEXT(derivedMethod);

    // Update exact context handle.
    //
    if (pExactContextHandle != nullptr)
    {
        *pExactContextHandle = MAKE_CLASSCONTEXT(derivedClass);
    }

#ifdef FEATURE_READYTORUN
    if (opts.IsReadyToRun())
    {
        // For R2R, getCallInfo triggers bookkeeping on the zap
        // side and acquires the actual symbol to call so we need to call it here.

        // Look up the new call info.
        CORINFO_CALL_INFO derivedCallInfo;
        eeGetCallInfo(pDerivedResolvedToken, nullptr, CORINFO_CALLINFO_ALLOWINSTPARAM, &derivedCallInfo);

        // Update the call.
        call->gtCallMoreFlags &= ~GTF_CALL_M_VIRTSTUB_REL_INDIRECT;
        call->gtCallMoreFlags &= ~GTF_CALL_M_R2R_REL_INDIRECT;
        call->setEntryPoint(derivedCallInfo.codePointerLookup.constLookup);
    }
#endif // FEATURE_READYTORUN
}

//------------------------------------------------------------------------
// impConsiderCallProbe: Consider whether a call should get a histogram probe
// and mark it if so.
//
// Arguments:
//     call - The call
//     ilOffset - The precise IL offset of the call
//
// Returns:
//     True if the call was marked such that we will add a class or method probe for it.
//
bool Compiler::impConsiderCallProbe(GenTreeCall* call, IL_OFFSET ilOffset)
{
    // Possibly instrument. Note for OSR+PGO we will instrument when
    // optimizing and (currently) won't devirtualize. We may want
    // to revisit -- if we can devirtualize we should be able to
    // suppress the probe.
    //
    // We strip BBINSTR from inlinees currently, so we'll only
    // do this for the root method calls.
    //
    if (!opts.jitFlags->IsSet(JitFlags::JIT_FLAG_BBINSTR))
    {
        return false;
    }

    assert(opts.OptimizationDisabled() || opts.IsOSR());
    assert(!compIsForInlining());

    // During importation, optionally flag this block as one that
    // contains calls requiring class profiling. Ideally perhaps
    // we'd just keep track of the calls themselves, so we don't
    // have to search for them later.
    //
    if (compClassifyGDVProbeType(call) == GDVProbeType::None)
    {
        return false;
    }

    JITDUMP("\n ... marking [%06u] in " FMT_BB " for method/class profile instrumentation\n", dspTreeID(call),
            compCurBB->bbNum);
    HandleHistogramProfileCandidateInfo* pInfo = new (this, CMK_Inlining) HandleHistogramProfileCandidateInfo;

    // Record some info needed for the class profiling probe.
    //
    pInfo->ilOffset                             = ilOffset;
    pInfo->probeIndex                           = info.compHandleHistogramProbeCount++;
    call->gtHandleHistogramProfileCandidateInfo = pInfo;

    // Flag block as needing scrutiny
    //
    compCurBB->bbFlags |= BBF_HAS_HISTOGRAM_PROFILE;
    return true;
}

//------------------------------------------------------------------------
// compClassifyGDVProbeType:
//   Classify the type of GDV probe to use for a call site.
//
// Arguments:
//     call - The call
//
// Returns:
//     The type of probe to use.
//
Compiler::GDVProbeType Compiler::compClassifyGDVProbeType(GenTreeCall* call)
{
    if (call->gtCallType == CT_INDIRECT)
    {
        return GDVProbeType::None;
    }

    if (!opts.jitFlags->IsSet(JitFlags::JIT_FLAG_BBINSTR) || opts.jitFlags->IsSet(JitFlags::JIT_FLAG_PREJIT))
    {
        return GDVProbeType::None;
    }

    bool createTypeHistogram = false;
    if (JitConfig.JitClassProfiling() > 0)
    {
        createTypeHistogram = call->IsVirtualStub() || call->IsVirtualVtable();

        // Cast helpers may conditionally (depending on whether the class is
        // exact or not) have probes. For those helpers we do not use this
        // function to classify the probe type until after we have decided on
        // whether we probe them or not.
        createTypeHistogram = createTypeHistogram || (impIsCastHelperEligibleForClassProbe(call) &&
                                                      (call->gtHandleHistogramProfileCandidateInfo != nullptr));
    }

    bool createMethodHistogram = ((JitConfig.JitDelegateProfiling() > 0) && call->IsDelegateInvoke()) ||
                                 ((JitConfig.JitVTableProfiling() > 0) && call->IsVirtualVtable());

    if (createTypeHistogram && createMethodHistogram)
    {
        return GDVProbeType::MethodAndClassProfile;
    }

    if (createTypeHistogram)
    {
        return GDVProbeType::ClassProfile;
    }

    if (createMethodHistogram)
    {
        return GDVProbeType::MethodProfile;
    }

    return GDVProbeType::None;
}

//------------------------------------------------------------------------
// impGetSpecialIntrinsicExactReturnType: Look for special cases where a call
//   to an intrinsic returns an exact type
//
// Arguments:
//     methodHnd -- handle for the special intrinsic method
//
// Returns:
//     Exact class handle returned by the intrinsic call, if known.
//     Nullptr if not known, or not likely to lead to beneficial optimization.
CORINFO_CLASS_HANDLE Compiler::impGetSpecialIntrinsicExactReturnType(GenTreeCall* call)
{
    CORINFO_METHOD_HANDLE methodHnd = call->gtCallMethHnd;
    JITDUMP("Special intrinsic: looking for exact type returned by %s\n", eeGetMethodFullName(methodHnd));

    CORINFO_CLASS_HANDLE result = nullptr;

    // See what intrinsic we have...
    const NamedIntrinsic ni = lookupNamedIntrinsic(methodHnd);
    switch (ni)
    {
        case NI_System_Collections_Generic_Comparer_get_Default:
        case NI_System_Collections_Generic_EqualityComparer_get_Default:
        {
            // Expect one class generic parameter; figure out which it is.
            CORINFO_SIG_INFO sig;
            info.compCompHnd->getMethodSig(methodHnd, &sig);
            assert(sig.sigInst.classInstCount == 1);
            CORINFO_CLASS_HANDLE typeHnd = sig.sigInst.classInst[0];
            assert(typeHnd != nullptr);

            // Lookup can incorrect when we have __Canon as it won't appear
            // to implement any interface types.
            //
            // And if we do not have a final type, devirt & inlining is
            // unlikely to result in much simplification.
            //
            // We can use CORINFO_FLG_FINAL to screen out both of these cases.
            const DWORD typeAttribs = info.compCompHnd->getClassAttribs(typeHnd);
            bool        isFinalType = ((typeAttribs & CORINFO_FLG_FINAL) != 0);

            if (!isFinalType)
            {
                CallArg* instParam = call->gtArgs.FindWellKnownArg(WellKnownArg::InstParam);
                if (instParam != nullptr)
                {
                    assert(instParam->GetNext() == nullptr);
                    CORINFO_CLASS_HANDLE hClass = gtGetHelperArgClassHandle(instParam->GetEarlyNode());
                    if (hClass != NO_CLASS_HANDLE)
                    {
                        hClass = getTypeInstantiationArgument(hClass, 0);
                        if ((info.compCompHnd->getClassAttribs(hClass) & CORINFO_FLG_FINAL) != 0)
                        {
                            typeHnd     = hClass;
                            isFinalType = true;
                        }
                    }
                }
            }

            if (isFinalType)
            {
                if (ni == NI_System_Collections_Generic_EqualityComparer_get_Default)
                {
                    result = info.compCompHnd->getDefaultEqualityComparerClass(typeHnd);
                }
                else
                {
                    assert(ni == NI_System_Collections_Generic_Comparer_get_Default);
                    result = info.compCompHnd->getDefaultComparerClass(typeHnd);
                }
                JITDUMP("Special intrinsic for type %s: return type is %s\n", eeGetClassName(typeHnd),
                        result != nullptr ? eeGetClassName(result) : "unknown");
            }
            else
            {
                JITDUMP("Special intrinsic for type %s: type not final, so deferring opt\n", eeGetClassName(typeHnd));
            }

            break;
        }

        default:
        {
            JITDUMP("This special intrinsic not handled, sorry...\n");
            break;
        }
    }

    return result;
}

//------------------------------------------------------------------------
// impTailCallRetTypeCompatible: Checks whether the return types of caller
//    and callee are compatible so that calle can be tail called.
//    sizes are not supported integral type sizes return values to temps.
//
// Arguments:
//     allowWidening -- whether to allow implicit widening by the callee.
//                      For instance, allowing int32 -> int16 tailcalls.
//                      The managed calling convention allows this, but
//                      we don't want explicit tailcalls to depend on this
//                      detail of the managed calling convention.
//     callerRetType -- the caller's return type
//     callerRetTypeClass - the caller's return struct type
//     callerCallConv -- calling convention of the caller
//     calleeRetType -- the callee's return type
//     calleeRetTypeClass - the callee return struct type
//     calleeCallConv -- calling convention of the callee
//
// Returns:
//     True if the tailcall types are compatible.
//
// Remarks:
//     Note that here we don't check compatibility in IL Verifier sense, but on the
//     lines of return types getting returned in the same return register.
bool Compiler::impTailCallRetTypeCompatible(bool                     allowWidening,
                                            var_types                callerRetType,
                                            CORINFO_CLASS_HANDLE     callerRetTypeClass,
                                            CorInfoCallConvExtension callerCallConv,
                                            var_types                calleeRetType,
                                            CORINFO_CLASS_HANDLE     calleeRetTypeClass,
                                            CorInfoCallConvExtension calleeCallConv)
{
    // Early out if the types are the same.
    if (callerRetType == calleeRetType)
    {
        return true;
    }

    // For integral types the managed calling convention dictates that callee
    // will widen the return value to 4 bytes, so we can allow implicit widening
    // in managed to managed tailcalls when dealing with <= 4 bytes.
    bool isManaged =
        (callerCallConv == CorInfoCallConvExtension::Managed) && (calleeCallConv == CorInfoCallConvExtension::Managed);

    if (allowWidening && isManaged && varTypeIsIntegral(callerRetType) && varTypeIsIntegral(calleeRetType) &&
        (genTypeSize(callerRetType) <= 4) && (genTypeSize(calleeRetType) <= genTypeSize(callerRetType)))
    {
        return true;
    }

    // If the class handles are the same and not null, the return types are compatible.
    if ((callerRetTypeClass != nullptr) && (callerRetTypeClass == calleeRetTypeClass))
    {
        return true;
    }

#if defined(TARGET_AMD64) || defined(TARGET_ARM64) || defined(TARGET_LOONGARCH64)
    // Jit64 compat:
    if (callerRetType == TYP_VOID)
    {
        // This needs to be allowed to support the following IL pattern that Jit64 allows:
        //     tail.call
        //     pop
        //     ret
        //
        // Note that the above IL pattern is not valid as per IL verification rules.
        // Therefore, only full trust code can take advantage of this pattern.
        return true;
    }

    // These checks return true if the return value type sizes are the same and
    // get returned in the same return register i.e. caller doesn't need to normalize
    // return value. Some of the tail calls permitted by below checks would have
    // been rejected by IL Verifier before we reached here.  Therefore, only full
    // trust code can make those tail calls.
    unsigned callerRetTypeSize = 0;
    unsigned calleeRetTypeSize = 0;
    bool isCallerRetTypMBEnreg = VarTypeIsMultiByteAndCanEnreg(callerRetType, callerRetTypeClass, &callerRetTypeSize,
                                                               true, info.compIsVarArgs, callerCallConv);
    bool isCalleeRetTypMBEnreg = VarTypeIsMultiByteAndCanEnreg(calleeRetType, calleeRetTypeClass, &calleeRetTypeSize,
                                                               true, info.compIsVarArgs, calleeCallConv);

    if (varTypeIsIntegral(callerRetType) || isCallerRetTypMBEnreg)
    {
        return (varTypeIsIntegral(calleeRetType) || isCalleeRetTypMBEnreg) && (callerRetTypeSize == calleeRetTypeSize);
    }
#endif // TARGET_AMD64 || TARGET_ARM64 || TARGET_LOONGARCH64

    return false;
}

//------------------------------------------------------------------------
// impCheckCanInline: do more detailed checks to determine if a method can
//   be inlined, and collect information that will be needed later
//
// Arguments:
//   call - inline candidate
//   fncHandle - method that will be called
//   methAttr - attributes for the method
//   exactContextHnd - exact context for the method
//   ppInlineCandidateInfo [out] - information needed later for inlining
//   inlineResult - result of ongoing inline evaluation
//
// Notes:
//   Will update inlineResult with observations and possible failure
//   status (if method cannot be inlined)
//
void Compiler::impCheckCanInline(GenTreeCall*           call,
                                 CORINFO_METHOD_HANDLE  fncHandle,
                                 unsigned               methAttr,
                                 CORINFO_CONTEXT_HANDLE exactContextHnd,
                                 InlineCandidateInfo**  ppInlineCandidateInfo,
                                 InlineResult*          inlineResult)
{
    // Either EE or JIT might throw exceptions below.
    // If that happens, just don't inline the method.
    //
    struct Param
    {
        Compiler*              pThis;
        GenTreeCall*           call;
        CORINFO_METHOD_HANDLE  fncHandle;
        unsigned               methAttr;
        CORINFO_CONTEXT_HANDLE exactContextHnd;
        InlineResult*          result;
        InlineCandidateInfo**  ppInlineCandidateInfo;
    } param;
    memset(&param, 0, sizeof(param));

    param.pThis                 = this;
    param.call                  = call;
    param.fncHandle             = fncHandle;
    param.methAttr              = methAttr;
    param.exactContextHnd       = (exactContextHnd != nullptr) ? exactContextHnd : MAKE_METHODCONTEXT(fncHandle);
    param.result                = inlineResult;
    param.ppInlineCandidateInfo = ppInlineCandidateInfo;

    bool success = eeRunWithErrorTrap<Param>(
        [](Param* pParam) {

            // Cache some frequently accessed state.
            //
            Compiler* const       compiler     = pParam->pThis;
            COMP_HANDLE           compCompHnd  = compiler->info.compCompHnd;
            CORINFO_METHOD_HANDLE ftn          = pParam->fncHandle;
            InlineResult* const   inlineResult = pParam->result;

#ifdef DEBUG
            if (JitConfig.JitNoInline())
            {
                inlineResult->NoteFatal(InlineObservation::CALLEE_IS_JIT_NOINLINE);
                return;
            }
#endif

            // Fetch method info. This may fail, if the method doesn't have IL.
            //
            CORINFO_METHOD_INFO methInfo;
            if (!compCompHnd->getMethodInfo(ftn, &methInfo))
            {
                inlineResult->NoteFatal(InlineObservation::CALLEE_NO_METHOD_INFO);
                return;
            }

            // Profile data allows us to avoid early "too many IL bytes" outs.
            //
            inlineResult->NoteBool(InlineObservation::CALLSITE_HAS_PROFILE, compiler->fgHaveSufficientProfileData());

            bool const forceInline = (pParam->methAttr & CORINFO_FLG_FORCEINLINE) != 0;

            compiler->impCanInlineIL(ftn, &methInfo, forceInline, inlineResult);

            if (inlineResult->IsFailure())
            {
                assert(inlineResult->IsNever());
                return;
            }

            // Speculatively check if initClass() can be done.
            // If it can be done, we will try to inline the method.
            CorInfoInitClassResult const initClassResult =
                compCompHnd->initClass(nullptr /* field */, ftn /* method */, pParam->exactContextHnd /* context */);

            if (initClassResult & CORINFO_INITCLASS_DONT_INLINE)
            {
                inlineResult->NoteFatal(InlineObservation::CALLSITE_CANT_CLASS_INIT);
                return;
            }

            // Given the VM the final say in whether to inline or not.
            // This should be last since for verifiable code, this can be expensive
            //
            CorInfoInline const vmResult = compCompHnd->canInline(compiler->info.compMethodHnd, ftn);

            if (vmResult == INLINE_FAIL)
            {
                inlineResult->NoteFatal(InlineObservation::CALLSITE_IS_VM_NOINLINE);
            }
            else if (vmResult == INLINE_NEVER)
            {
                inlineResult->NoteFatal(InlineObservation::CALLEE_IS_VM_NOINLINE);
            }

            if (inlineResult->IsFailure())
            {
                // The VM already self-reported this failure, so mark it specially
                // so the JIT doesn't also try reporting it.
                //
                inlineResult->SetVMFailure();
                return;
            }

            // Get the method's class properties
            //
            CORINFO_CLASS_HANDLE clsHandle = compCompHnd->getMethodClass(ftn);
            unsigned const       clsAttr   = compCompHnd->getClassAttribs(clsHandle);

            // Return type
            //
            var_types const fncRetType = pParam->call->TypeGet();

#ifdef DEBUG
            var_types fncRealRetType = JITtype2varType(methInfo.args.retType);

            assert((genActualType(fncRealRetType) == genActualType(fncRetType)) ||
                   // <BUGNUM> VSW 288602 </BUGNUM>
                   // In case of IJW, we allow to assign a native pointer to a BYREF.
                   (fncRetType == TYP_BYREF && methInfo.args.retType == CORINFO_TYPE_PTR) ||
                   (varTypeIsStruct(fncRetType) && (fncRealRetType == TYP_STRUCT)));
#endif

            // Allocate an InlineCandidateInfo structure,
            //
            // Or, reuse the existing GuardedDevirtualizationCandidateInfo,
            // which was pre-allocated to have extra room.
            //
            InlineCandidateInfo* pInfo;

            if (pParam->call->IsGuardedDevirtualizationCandidate())
            {
                pInfo = pParam->call->gtInlineCandidateInfo;
            }
            else
            {
                pInfo = new (pParam->pThis, CMK_Inlining) InlineCandidateInfo;

                // Null out bits we don't use when we're just inlining
                //
                pInfo->guardedClassHandle              = nullptr;
                pInfo->guardedMethodHandle             = nullptr;
                pInfo->guardedMethodUnboxedEntryHandle = nullptr;
                pInfo->likelihood                      = 0;
                pInfo->requiresInstMethodTableArg      = false;
            }

            pInfo->methInfo                       = methInfo;
            pInfo->ilCallerHandle                 = pParam->pThis->info.compMethodHnd;
            pInfo->clsHandle                      = clsHandle;
            pInfo->exactContextHnd                = pParam->exactContextHnd;
            pInfo->retExpr                        = nullptr;
            pInfo->preexistingSpillTemp           = BAD_VAR_NUM;
            pInfo->clsAttr                        = clsAttr;
            pInfo->methAttr                       = pParam->methAttr;
            pInfo->initClassResult                = initClassResult;
            pInfo->fncRetType                     = fncRetType;
            pInfo->exactContextNeedsRuntimeLookup = false;
            pInfo->inlinersContext                = pParam->pThis->compInlineContext;

            // Note exactContextNeedsRuntimeLookup is reset later on,
            // over in impMarkInlineCandidate.
            //
            *(pParam->ppInlineCandidateInfo) = pInfo;
        },
        &param);

    if (!success)
    {
        inlineResult->NoteFatal(InlineObservation::CALLSITE_COMPILATION_ERROR);
    }
}

GenTree* Compiler::impMathIntrinsic(CORINFO_METHOD_HANDLE method,
                                    CORINFO_SIG_INFO*     sig,
                                    var_types             callType,
                                    NamedIntrinsic        intrinsicName,
                                    bool                  tailCall)
{
    GenTree* op1;
    GenTree* op2;

    assert(callType != TYP_STRUCT);
    assert(IsMathIntrinsic(intrinsicName));

    op1 = nullptr;

#if !defined(TARGET_X86)
    // Intrinsics that are not implemented directly by target instructions will
    // be re-materialized as users calls in rationalizer. For prefixed tail calls,
    // don't do this optimization, because
    //  a) For back compatibility reasons on desktop .NET Framework 4.6 / 4.6.1
    //  b) It will be non-trivial task or too late to re-materialize a surviving
    //     tail prefixed GT_INTRINSIC as tail call in rationalizer.
    if (!IsIntrinsicImplementedByUserCall(intrinsicName) || !tailCall)
#else
    // On x86 RyuJIT, importing intrinsics that are implemented as user calls can cause incorrect calculation
    // of the depth of the stack if these intrinsics are used as arguments to another call. This causes bad
    // code generation for certain EH constructs.
    if (!IsIntrinsicImplementedByUserCall(intrinsicName))
#endif
    {
        CORINFO_CLASS_HANDLE    tmpClass;
        CORINFO_ARG_LIST_HANDLE arg;
        var_types               op1Type;
        var_types               op2Type;

        switch (sig->numArgs)
        {
            case 1:
                op1 = impPopStack().val;

                arg     = sig->args;
                op1Type = JITtype2varType(strip(info.compCompHnd->getArgType(sig, arg, &tmpClass)));

                if (op1->TypeGet() != genActualType(op1Type))
                {
                    assert(varTypeIsFloating(op1));
                    op1 = gtNewCastNode(callType, op1, false, callType);
                }

                op1 = new (this, GT_INTRINSIC) GenTreeIntrinsic(genActualType(callType), op1, intrinsicName, method);
                break;

            case 2:
                op2 = impPopStack().val;
                op1 = impPopStack().val;

                arg     = sig->args;
                op1Type = JITtype2varType(strip(info.compCompHnd->getArgType(sig, arg, &tmpClass)));

                if (op1->TypeGet() != genActualType(op1Type))
                {
                    assert(varTypeIsFloating(op1));
                    op1 = gtNewCastNode(callType, op1, false, callType);
                }

                arg     = info.compCompHnd->getArgNext(arg);
                op2Type = JITtype2varType(strip(info.compCompHnd->getArgType(sig, arg, &tmpClass)));

                if (op2->TypeGet() != genActualType(op2Type))
                {
                    assert(varTypeIsFloating(op2));
                    op2 = gtNewCastNode(callType, op2, false, callType);
                }

                op1 =
                    new (this, GT_INTRINSIC) GenTreeIntrinsic(genActualType(callType), op1, op2, intrinsicName, method);
                break;

            default:
                NO_WAY("Unsupported number of args for Math Intrinsic");
        }

        if (IsIntrinsicImplementedByUserCall(intrinsicName))
        {
            op1->gtFlags |= GTF_CALL;
        }
    }

    return op1;
}

//------------------------------------------------------------------------
// lookupNamedIntrinsic: map method to jit named intrinsic value
//
// Arguments:
//    method -- method handle for method
//
// Return Value:
//    Id for the named intrinsic, or Illegal if none.
//
// Notes:
//    method should have CORINFO_FLG_INTRINSIC set in its attributes,
//    otherwise it is not a named jit intrinsic.
//
NamedIntrinsic Compiler::lookupNamedIntrinsic(CORINFO_METHOD_HANDLE method)
{
    const char* className          = nullptr;
    const char* namespaceName      = nullptr;
    const char* enclosingClassName = nullptr;
    const char* methodName =
        info.compCompHnd->getMethodNameFromMetadata(method, &className, &namespaceName, &enclosingClassName);

    JITDUMP("Named Intrinsic ");

    if (namespaceName != nullptr)
    {
        JITDUMP("%s.", namespaceName);
    }
    if (enclosingClassName != nullptr)
    {
        JITDUMP("%s.", enclosingClassName);
    }
    if (className != nullptr)
    {
        JITDUMP("%s.", className);
    }
    if (methodName != nullptr)
    {
        JITDUMP("%s", methodName);
    }

    if ((namespaceName == nullptr) || (className == nullptr) || (methodName == nullptr))
    {
        // Check if we are dealing with an MD array's known runtime method
        CorInfoArrayIntrinsic arrayFuncIndex = info.compCompHnd->getArrayIntrinsicID(method);
        switch (arrayFuncIndex)
        {
            case CorInfoArrayIntrinsic::GET:
                JITDUMP("ARRAY_FUNC_GET: Recognized\n");
                return NI_Array_Get;
            case CorInfoArrayIntrinsic::SET:
                JITDUMP("ARRAY_FUNC_SET: Recognized\n");
                return NI_Array_Set;
            case CorInfoArrayIntrinsic::ADDRESS:
                JITDUMP("ARRAY_FUNC_ADDRESS: Recognized\n");
                return NI_Array_Address;
            default:
                break;
        }

        JITDUMP(": Not recognized, not enough metadata\n");
        return NI_Illegal;
    }

    JITDUMP(": ");

    NamedIntrinsic result = NI_Illegal;

    if (strcmp(namespaceName, "System") == 0)
    {
        if ((strcmp(className, "Enum") == 0) && (strcmp(methodName, "HasFlag") == 0))
        {
            result = NI_System_Enum_HasFlag;
        }
        else if (strcmp(className, "Activator") == 0)
        {
            if (strcmp(methodName, "AllocatorOf") == 0)
            {
                result = NI_System_Activator_AllocatorOf;
            }
            else if (strcmp(methodName, "DefaultConstructorOf") == 0)
            {
                result = NI_System_Activator_DefaultConstructorOf;
            }
        }
        else if (strcmp(className, "BitConverter") == 0)
        {
            if (methodName[0] == 'D')
            {
                if (strcmp(methodName, "DoubleToInt64Bits") == 0)
                {
                    result = NI_System_BitConverter_DoubleToInt64Bits;
                }
                else if (strcmp(methodName, "DoubleToUInt64Bits") == 0)
                {
                    result = NI_System_BitConverter_DoubleToInt64Bits;
                }
            }
            else if (methodName[0] == 'I')
            {
                if (strcmp(methodName, "Int32BitsToSingle") == 0)
                {
                    result = NI_System_BitConverter_Int32BitsToSingle;
                }
                else if (strcmp(methodName, "Int64BitsToDouble") == 0)
                {
                    result = NI_System_BitConverter_Int64BitsToDouble;
                }
            }
            else if (methodName[0] == 'S')
            {
                if (strcmp(methodName, "SingleToInt32Bits") == 0)
                {
                    result = NI_System_BitConverter_SingleToInt32Bits;
                }
                else if (strcmp(methodName, "SingleToUInt32Bits") == 0)
                {
                    result = NI_System_BitConverter_SingleToInt32Bits;
                }
            }
            else if (methodName[0] == 'U')
            {
                if (strcmp(methodName, "UInt32BitsToSingle") == 0)
                {
                    result = NI_System_BitConverter_Int32BitsToSingle;
                }
                else if (strcmp(methodName, "UInt64BitsToDouble") == 0)
                {
                    result = NI_System_BitConverter_Int64BitsToDouble;
                }
            }
        }
        else if (strcmp(className, "Math") == 0 || strcmp(className, "MathF") == 0)
        {
            if (strcmp(methodName, "Abs") == 0)
            {
                result = NI_System_Math_Abs;
            }
            else if (strcmp(methodName, "Acos") == 0)
            {
                result = NI_System_Math_Acos;
            }
            else if (strcmp(methodName, "Acosh") == 0)
            {
                result = NI_System_Math_Acosh;
            }
            else if (strcmp(methodName, "Asin") == 0)
            {
                result = NI_System_Math_Asin;
            }
            else if (strcmp(methodName, "Asinh") == 0)
            {
                result = NI_System_Math_Asinh;
            }
            else if (strcmp(methodName, "Atan") == 0)
            {
                result = NI_System_Math_Atan;
            }
            else if (strcmp(methodName, "Atanh") == 0)
            {
                result = NI_System_Math_Atanh;
            }
            else if (strcmp(methodName, "Atan2") == 0)
            {
                result = NI_System_Math_Atan2;
            }
            else if (strcmp(methodName, "Cbrt") == 0)
            {
                result = NI_System_Math_Cbrt;
            }
            else if (strcmp(methodName, "Ceiling") == 0)
            {
                result = NI_System_Math_Ceiling;
            }
            else if (strcmp(methodName, "Cos") == 0)
            {
                result = NI_System_Math_Cos;
            }
            else if (strcmp(methodName, "Cosh") == 0)
            {
                result = NI_System_Math_Cosh;
            }
            else if (strcmp(methodName, "Exp") == 0)
            {
                result = NI_System_Math_Exp;
            }
            else if (strcmp(methodName, "Floor") == 0)
            {
                result = NI_System_Math_Floor;
            }
            else if (strcmp(methodName, "FMod") == 0)
            {
                result = NI_System_Math_FMod;
            }
            else if (strcmp(methodName, "FusedMultiplyAdd") == 0)
            {
                result = NI_System_Math_FusedMultiplyAdd;
            }
            else if (strcmp(methodName, "ILogB") == 0)
            {
                result = NI_System_Math_ILogB;
            }
            else if (strcmp(methodName, "Log") == 0)
            {
                result = NI_System_Math_Log;
            }
            else if (strcmp(methodName, "Log2") == 0)
            {
                result = NI_System_Math_Log2;
            }
            else if (strcmp(methodName, "Log10") == 0)
            {
                result = NI_System_Math_Log10;
            }
            else if (strcmp(methodName, "Max") == 0)
            {
                result = NI_System_Math_Max;
            }
            else if (strcmp(methodName, "Min") == 0)
            {
                result = NI_System_Math_Min;
            }
            else if (strcmp(methodName, "Pow") == 0)
            {
                result = NI_System_Math_Pow;
            }
            else if (strcmp(methodName, "Round") == 0)
            {
                result = NI_System_Math_Round;
            }
            else if (strcmp(methodName, "Sin") == 0)
            {
                result = NI_System_Math_Sin;
            }
            else if (strcmp(methodName, "Sinh") == 0)
            {
                result = NI_System_Math_Sinh;
            }
            else if (strcmp(methodName, "Sqrt") == 0)
            {
                result = NI_System_Math_Sqrt;
            }
            else if (strcmp(methodName, "Tan") == 0)
            {
                result = NI_System_Math_Tan;
            }
            else if (strcmp(methodName, "Tanh") == 0)
            {
                result = NI_System_Math_Tanh;
            }
            else if (strcmp(methodName, "Truncate") == 0)
            {
                result = NI_System_Math_Truncate;
            }
        }
        else if (strcmp(className, "GC") == 0)
        {
            if (strcmp(methodName, "KeepAlive") == 0)
            {
                result = NI_System_GC_KeepAlive;
            }
        }
        else if (strcmp(className, "Array") == 0)
        {
            if (strcmp(methodName, "Clone") == 0)
            {
                result = NI_System_Array_Clone;
            }
            else if (strcmp(methodName, "GetLength") == 0)
            {
                result = NI_System_Array_GetLength;
            }
            else if (strcmp(methodName, "GetLowerBound") == 0)
            {
                result = NI_System_Array_GetLowerBound;
            }
            else if (strcmp(methodName, "GetUpperBound") == 0)
            {
                result = NI_System_Array_GetUpperBound;
            }
        }
        else if (strcmp(className, "Object") == 0)
        {
            if (strcmp(methodName, "MemberwiseClone") == 0)
            {
                result = NI_System_Object_MemberwiseClone;
            }
            else if (strcmp(methodName, "GetType") == 0)
            {
                result = NI_System_Object_GetType;
            }
        }
        else if (strcmp(className, "RuntimeTypeHandle") == 0)
        {
            if (strcmp(methodName, "GetValueInternal") == 0)
            {
                result = NI_System_RuntimeTypeHandle_GetValueInternal;
            }
        }
        else if (strcmp(className, "Type") == 0)
        {
            if (strcmp(methodName, "get_IsValueType") == 0)
            {
                result = NI_System_Type_get_IsValueType;
            }
            else if (strcmp(methodName, "get_IsByRefLike") == 0)
            {
                result = NI_System_Type_get_IsByRefLike;
            }
            else if (strcmp(methodName, "IsAssignableFrom") == 0)
            {
                result = NI_System_Type_IsAssignableFrom;
            }
            else if (strcmp(methodName, "IsAssignableTo") == 0)
            {
                result = NI_System_Type_IsAssignableTo;
            }
            else if (strcmp(methodName, "op_Equality") == 0)
            {
                result = NI_System_Type_op_Equality;
            }
            else if (strcmp(methodName, "op_Inequality") == 0)
            {
                result = NI_System_Type_op_Inequality;
            }
            else if (strcmp(methodName, "GetTypeFromHandle") == 0)
            {
                result = NI_System_Type_GetTypeFromHandle;
            }
        }
        else if (strcmp(className, "String") == 0)
        {
            if (strcmp(methodName, "Equals") == 0)
            {
                result = NI_System_String_Equals;
            }
            else if (strcmp(methodName, "get_Chars") == 0)
            {
                result = NI_System_String_get_Chars;
            }
            else if (strcmp(methodName, "get_Length") == 0)
            {
                result = NI_System_String_get_Length;
            }
            else if (strcmp(methodName, "op_Implicit") == 0)
            {
                result = NI_System_String_op_Implicit;
            }
            else if (strcmp(methodName, "StartsWith") == 0)
            {
                result = NI_System_String_StartsWith;
            }
        }
        else if (strcmp(className, "MemoryExtensions") == 0)
        {
            if (strcmp(methodName, "AsSpan") == 0)
            {
                result = NI_System_MemoryExtensions_AsSpan;
            }
            if (strcmp(methodName, "SequenceEqual") == 0)
            {
                result = NI_System_MemoryExtensions_SequenceEqual;
            }
            else if (strcmp(methodName, "Equals") == 0)
            {
                result = NI_System_MemoryExtensions_Equals;
            }
            else if (strcmp(methodName, "StartsWith") == 0)
            {
                result = NI_System_MemoryExtensions_StartsWith;
            }
        }
        else if (strcmp(className, "Span`1") == 0)
        {
            if (strcmp(methodName, "get_Item") == 0)
            {
                result = NI_System_Span_get_Item;
            }
        }
        else if (strcmp(className, "ReadOnlySpan`1") == 0)
        {
            if (strcmp(methodName, "get_Item") == 0)
            {
                result = NI_System_ReadOnlySpan_get_Item;
            }
        }
        else if (strcmp(className, "EETypePtr") == 0)
        {
            if (strcmp(methodName, "EETypePtrOf") == 0)
            {
                result = NI_System_EETypePtr_EETypePtrOf;
            }
        }
    }
    else if (strcmp(namespaceName, "Internal.Runtime") == 0)
    {
        if (strcmp(className, "MethodTable") == 0)
        {
            if (strcmp(methodName, "Of") == 0)
            {
                result = NI_Internal_Runtime_MethodTable_Of;
            }
        }
    }
    else if (strcmp(namespaceName, "System.Threading") == 0)
    {
        if (strcmp(className, "Thread") == 0)
        {
            if (strcmp(methodName, "get_CurrentThread") == 0)
            {
                result = NI_System_Threading_Thread_get_CurrentThread;
            }
            else if (strcmp(methodName, "get_ManagedThreadId") == 0)
            {
                result = NI_System_Threading_Thread_get_ManagedThreadId;
            }
        }
        else if (strcmp(className, "Interlocked") == 0)
        {
#ifndef TARGET_ARM64
            // TODO-CQ: Implement for XArch (https://github.com/dotnet/runtime/issues/32239).
            if (strcmp(methodName, "And") == 0)
            {
                result = NI_System_Threading_Interlocked_And;
            }
            else if (strcmp(methodName, "Or") == 0)
            {
                result = NI_System_Threading_Interlocked_Or;
            }
#endif
            if (strcmp(methodName, "CompareExchange") == 0)
            {
                result = NI_System_Threading_Interlocked_CompareExchange;
            }
            else if (strcmp(methodName, "Exchange") == 0)
            {
                result = NI_System_Threading_Interlocked_Exchange;
            }
            else if (strcmp(methodName, "ExchangeAdd") == 0)
            {
                result = NI_System_Threading_Interlocked_ExchangeAdd;
            }
            else if (strcmp(methodName, "MemoryBarrier") == 0)
            {
                result = NI_System_Threading_Interlocked_MemoryBarrier;
            }
            else if (strcmp(methodName, "ReadMemoryBarrier") == 0)
            {
                result = NI_System_Threading_Interlocked_ReadMemoryBarrier;
            }
        }
    }
#if defined(TARGET_XARCH) || defined(TARGET_ARM64)
    else if (strcmp(namespaceName, "System.Buffers.Binary") == 0)
    {
        if ((strcmp(className, "BinaryPrimitives") == 0) && (strcmp(methodName, "ReverseEndianness") == 0))
        {
            result = NI_System_Buffers_Binary_BinaryPrimitives_ReverseEndianness;
        }
    }
#endif // defined(TARGET_XARCH) || defined(TARGET_ARM64)
    else if (strcmp(namespaceName, "System.Collections.Generic") == 0)
    {
        if ((strcmp(className, "EqualityComparer`1") == 0) && (strcmp(methodName, "get_Default") == 0))
        {
            result = NI_System_Collections_Generic_EqualityComparer_get_Default;
        }
        else if ((strcmp(className, "Comparer`1") == 0) && (strcmp(methodName, "get_Default") == 0))
        {
            result = NI_System_Collections_Generic_Comparer_get_Default;
        }
    }
    else if ((strcmp(namespaceName, "System.Numerics") == 0) && (strcmp(className, "BitOperations") == 0))
    {
        if (strcmp(methodName, "PopCount") == 0)
        {
            result = NI_System_Numerics_BitOperations_PopCount;
        }
    }
    else if (strcmp(namespaceName, "System.Numerics") == 0)
    {
#ifdef FEATURE_HW_INTRINSICS
        CORINFO_SIG_INFO sig;
        info.compCompHnd->getMethodSig(method, &sig);

        int sizeOfVectorT = getSIMDVectorRegisterByteLength();

        result = SimdAsHWIntrinsicInfo::lookupId(this, &sig, className, methodName, enclosingClassName, sizeOfVectorT);
#endif // FEATURE_HW_INTRINSICS

        if (result == NI_Illegal)
        {
            if (strcmp(methodName, "get_IsHardwareAccelerated") == 0)
            {
                // This allows the relevant code paths to be dropped as dead code even
                // on platforms where FEATURE_HW_INTRINSICS is not supported.

                result = NI_IsSupported_False;
            }
            else if (gtIsRecursiveCall(method))
            {
                // For the framework itself, any recursive intrinsics will either be
                // only supported on a single platform or will be guarded by a relevant
                // IsSupported check so the throw PNSE will be valid or dropped.

                result = NI_Throw_PlatformNotSupportedException;
            }
        }
    }
    else if (strcmp(namespaceName, "System.Runtime.CompilerServices") == 0)
    {
        if (strcmp(className, "Unsafe") == 0)
        {
            if (strcmp(methodName, "Add") == 0)
            {
                result = NI_SRCS_UNSAFE_Add;
            }
            else if (strcmp(methodName, "AddByteOffset") == 0)
            {
                result = NI_SRCS_UNSAFE_AddByteOffset;
            }
            else if (strcmp(methodName, "AreSame") == 0)
            {
                result = NI_SRCS_UNSAFE_AreSame;
            }
            else if (strcmp(methodName, "As") == 0)
            {
                result = NI_SRCS_UNSAFE_As;
            }
            else if (strcmp(methodName, "AsPointer") == 0)
            {
                result = NI_SRCS_UNSAFE_AsPointer;
            }
            else if (strcmp(methodName, "AsRef") == 0)
            {
                result = NI_SRCS_UNSAFE_AsRef;
            }
            else if (strcmp(methodName, "ByteOffset") == 0)
            {
                result = NI_SRCS_UNSAFE_ByteOffset;
            }
            else if (strcmp(methodName, "Copy") == 0)
            {
                result = NI_SRCS_UNSAFE_Copy;
            }
            else if (strcmp(methodName, "CopyBlock") == 0)
            {
                result = NI_SRCS_UNSAFE_CopyBlock;
            }
            else if (strcmp(methodName, "CopyBlockUnaligned") == 0)
            {
                result = NI_SRCS_UNSAFE_CopyBlockUnaligned;
            }
            else if (strcmp(methodName, "InitBlock") == 0)
            {
                result = NI_SRCS_UNSAFE_InitBlock;
            }
            else if (strcmp(methodName, "InitBlockUnaligned") == 0)
            {
                result = NI_SRCS_UNSAFE_InitBlockUnaligned;
            }
            else if (strcmp(methodName, "IsAddressGreaterThan") == 0)
            {
                result = NI_SRCS_UNSAFE_IsAddressGreaterThan;
            }
            else if (strcmp(methodName, "IsAddressLessThan") == 0)
            {
                result = NI_SRCS_UNSAFE_IsAddressLessThan;
            }
            else if (strcmp(methodName, "IsNullRef") == 0)
            {
                result = NI_SRCS_UNSAFE_IsNullRef;
            }
            else if (strcmp(methodName, "NullRef") == 0)
            {
                result = NI_SRCS_UNSAFE_NullRef;
            }
            else if (strcmp(methodName, "Read") == 0)
            {
                result = NI_SRCS_UNSAFE_Read;
            }
            else if (strcmp(methodName, "ReadUnaligned") == 0)
            {
                result = NI_SRCS_UNSAFE_ReadUnaligned;
            }
            else if (strcmp(methodName, "SizeOf") == 0)
            {
                result = NI_SRCS_UNSAFE_SizeOf;
            }
            else if (strcmp(methodName, "SkipInit") == 0)
            {
                result = NI_SRCS_UNSAFE_SkipInit;
            }
            else if (strcmp(methodName, "Subtract") == 0)
            {
                result = NI_SRCS_UNSAFE_Subtract;
            }
            else if (strcmp(methodName, "SubtractByteOffset") == 0)
            {
                result = NI_SRCS_UNSAFE_SubtractByteOffset;
            }
            else if (strcmp(methodName, "Unbox") == 0)
            {
                result = NI_SRCS_UNSAFE_Unbox;
            }
            else if (strcmp(methodName, "Write") == 0)
            {
                result = NI_SRCS_UNSAFE_Write;
            }
            else if (strcmp(methodName, "WriteUnaligned") == 0)
            {
                result = NI_SRCS_UNSAFE_WriteUnaligned;
            }
        }
        else if (strcmp(className, "RuntimeHelpers") == 0)
        {
            if (strcmp(methodName, "CreateSpan") == 0)
            {
                result = NI_System_Runtime_CompilerServices_RuntimeHelpers_CreateSpan;
            }
            else if (strcmp(methodName, "InitializeArray") == 0)
            {
                result = NI_System_Runtime_CompilerServices_RuntimeHelpers_InitializeArray;
            }
            else if (strcmp(methodName, "IsKnownConstant") == 0)
            {
                result = NI_System_Runtime_CompilerServices_RuntimeHelpers_IsKnownConstant;
            }
        }
    }
    else if (strncmp(namespaceName, "System.Runtime.Intrinsics", 25) == 0)
    {
        // We go down this path even when FEATURE_HW_INTRINSICS isn't enabled
        // so we can specially handle IsSupported and recursive calls.

        // This is required to appropriately handle the intrinsics on platforms
        // which don't support them. On such a platform methods like Vector64.Create
        // will be seen as `Intrinsic` and `mustExpand` due to having a code path
        // which is recursive. When such a path is hit we expect it to be handled by
        // the importer and we fire an assert if it wasn't and in previous versions
        // of the JIT would fail fast. This was changed to throw a PNSE instead but
        // we still assert as most intrinsics should have been recognized/handled.

        // In order to avoid the assert, we specially handle the IsSupported checks
        // (to better allow dead-code optimizations) and we explicitly throw a PNSE
        // as we know that is the desired behavior for the HWIntrinsics when not
        // supported. For cases like Vector64.Create, this is fine because it will
        // be behind a relevant IsSupported check and will never be hit and the
        // software fallback will be executed instead.

        CLANG_FORMAT_COMMENT_ANCHOR;

#ifdef FEATURE_HW_INTRINSICS
        namespaceName += 25;
        const char* platformNamespaceName;

#if defined(TARGET_XARCH)
        platformNamespaceName = ".X86";
#elif defined(TARGET_ARM64)
        platformNamespaceName = ".Arm";
#else
#error Unsupported platform
#endif

        if ((namespaceName[0] == '\0') || (strcmp(namespaceName, platformNamespaceName) == 0))
        {
            CORINFO_SIG_INFO sig;
            info.compCompHnd->getMethodSig(method, &sig);

            result = HWIntrinsicInfo::lookupId(this, &sig, className, methodName, enclosingClassName);
        }
#endif // FEATURE_HW_INTRINSICS

        if (result == NI_Illegal)
        {
            if ((strcmp(methodName, "get_IsSupported") == 0) || (strcmp(methodName, "get_IsHardwareAccelerated") == 0))
            {
                // This allows the relevant code paths to be dropped as dead code even
                // on platforms where FEATURE_HW_INTRINSICS is not supported.

                result = NI_IsSupported_False;
            }
            else if (gtIsRecursiveCall(method))
            {
                // For the framework itself, any recursive intrinsics will either be
                // only supported on a single platform or will be guarded by a relevant
                // IsSupported check so the throw PNSE will be valid or dropped.

                result = NI_Throw_PlatformNotSupportedException;
            }
        }
    }
    else if (strcmp(namespaceName, "System.StubHelpers") == 0)
    {
        if (strcmp(className, "StubHelpers") == 0)
        {
            if (strcmp(methodName, "GetStubContext") == 0)
            {
                result = NI_System_StubHelpers_GetStubContext;
            }
            else if (strcmp(methodName, "NextCallReturnAddress") == 0)
            {
                result = NI_System_StubHelpers_NextCallReturnAddress;
            }
        }
    }

    if (result == NI_Illegal)
    {
        JITDUMP("Not recognized\n");
    }
    else if (result == NI_IsSupported_False)
    {
        JITDUMP("Unsupported - return false");
    }
    else if (result == NI_Throw_PlatformNotSupportedException)
    {
        JITDUMP("Unsupported - throw PlatformNotSupportedException");
    }
    else
    {
        JITDUMP("Recognized\n");
    }
    return result;
}

//------------------------------------------------------------------------
// impUnsupportedNamedIntrinsic: Throws an exception for an unsupported named intrinsic
//
// Arguments:
//    helper     - JIT helper ID for the exception to be thrown
//    method     - method handle of the intrinsic function.
//    sig        - signature of the intrinsic call
//    mustExpand - true if the intrinsic must return a GenTree*; otherwise, false
//
// Return Value:
//    a gtNewMustThrowException if mustExpand is true; otherwise, nullptr
//
GenTree* Compiler::impUnsupportedNamedIntrinsic(unsigned              helper,
                                                CORINFO_METHOD_HANDLE method,
                                                CORINFO_SIG_INFO*     sig,
                                                bool                  mustExpand)
{
    // We've hit some error case and may need to return a node for the given error.
    //
    // When `mustExpand=false`, we are attempting to inline the intrinsic directly into another method. In this
    // scenario, we need to return `nullptr` so that a GT_CALL to the intrinsic is emitted instead. This is to
    // ensure that everything continues to behave correctly when optimizations are enabled (e.g. things like the
    // inliner may expect the node we return to have a certain signature, and the `MustThrowException` node won't
    // match that).
    //
    // When `mustExpand=true`, we are in a GT_CALL to the intrinsic and are attempting to JIT it. This will generally
    // be in response to an indirect call (e.g. done via reflection) or in response to an earlier attempt returning
    // `nullptr` (under `mustExpand=false`). In that scenario, we are safe to return the `MustThrowException` node.

    if (mustExpand)
    {
        for (unsigned i = 0; i < sig->numArgs; i++)
        {
            impPopStack();
        }

        return gtNewMustThrowException(helper, JITtype2varType(sig->retType), sig->retTypeClass);
    }
    else
    {
        return nullptr;
    }
}

//------------------------------------------------------------------------
// impArrayAccessIntrinsic: try to replace a multi-dimensional array intrinsics with IR nodes.
//
// Arguments:
//    clsHnd        - handle for the intrinsic method's class
//    sig           - signature of the intrinsic method
//    memberRef     - the token for the intrinsic method
//    readonlyCall  - true if call has a readonly prefix
//    intrinsicName - the intrinsic to expand: one of NI_Array_Address, NI_Array_Get, NI_Array_Set
//
// Return Value:
//    The intrinsic expansion, or nullptr if the expansion was not done (and a function call should be made instead).
//
GenTree* Compiler::impArrayAccessIntrinsic(
    CORINFO_CLASS_HANDLE clsHnd, CORINFO_SIG_INFO* sig, int memberRef, bool readonlyCall, NamedIntrinsic intrinsicName)
{
    assert((intrinsicName == NI_Array_Address) || (intrinsicName == NI_Array_Get) || (intrinsicName == NI_Array_Set));

    // If we are generating SMALL_CODE, we don't want to use intrinsics, as it generates fatter code.
    if (compCodeOpt() == SMALL_CODE)
    {
        JITDUMP("impArrayAccessIntrinsic: rejecting array intrinsic due to SMALL_CODE\n");
        return nullptr;
    }

    unsigned rank = (intrinsicName == NI_Array_Set) ? (sig->numArgs - 1) : sig->numArgs;

    // Handle a maximum rank of GT_ARR_MAX_RANK (3). This is an implementation choice (larger ranks are expected
    // to be rare) and could be increased.
    if (rank > GT_ARR_MAX_RANK)
    {
        JITDUMP("impArrayAccessIntrinsic: rejecting array intrinsic because rank (%d) > GT_ARR_MAX_RANK (%d)\n", rank,
                GT_ARR_MAX_RANK);
        return nullptr;
    }

    // The rank 1 case is special because it has to handle two array formats. We will simply not do that case.
    if (rank <= 1)
    {
        JITDUMP("impArrayAccessIntrinsic: rejecting array intrinsic because rank (%d) <= 1\n", rank);
        return nullptr;
    }

    CORINFO_CLASS_HANDLE arrElemClsHnd = nullptr;
    var_types            elemType      = JITtype2varType(info.compCompHnd->getChildType(clsHnd, &arrElemClsHnd));

    // For the ref case, we will only be able to inline if the types match
    // (verifier checks for this, we don't care for the nonverified case and the
    // type is final (so we don't need to do the cast))
    if ((intrinsicName != NI_Array_Get) && !readonlyCall && varTypeIsGC(elemType))
    {
        // Get the call site signature
        CORINFO_SIG_INFO LocalSig;
        eeGetCallSiteSig(memberRef, info.compScopeHnd, impTokenLookupContextHandle, &LocalSig);
        assert(LocalSig.hasThis());

        CORINFO_CLASS_HANDLE actualElemClsHnd;

        if (intrinsicName == NI_Array_Set)
        {
            // Fetch the last argument, the one that indicates the type we are setting.
            CORINFO_ARG_LIST_HANDLE argType = LocalSig.args;
            for (unsigned r = 0; r < rank; r++)
            {
                argType = info.compCompHnd->getArgNext(argType);
            }

            typeInfo argInfo = verParseArgSigToTypeInfo(&LocalSig, argType);
            actualElemClsHnd = argInfo.GetClassHandle();
        }
        else
        {
            assert(intrinsicName == NI_Array_Address);

            // Fetch the return type
            typeInfo retInfo = verMakeTypeInfo(LocalSig.retType, LocalSig.retTypeClass);
            assert(retInfo.IsByRef());
            actualElemClsHnd = retInfo.GetClassHandle();
        }

        // if it's not final, we can't do the optimization
        if (!(info.compCompHnd->getClassAttribs(actualElemClsHnd) & CORINFO_FLG_FINAL))
        {
            JITDUMP("impArrayAccessIntrinsic: rejecting array intrinsic because actualElemClsHnd (%p) is not final\n",
                    dspPtr(actualElemClsHnd));
            return nullptr;
        }
    }

    unsigned arrayElemSize;
    if (elemType == TYP_STRUCT)
    {
        assert(arrElemClsHnd);
        arrayElemSize = info.compCompHnd->getClassSize(arrElemClsHnd);
    }
    else
    {
        arrayElemSize = genTypeSize(elemType);
    }

    if ((unsigned char)arrayElemSize != arrayElemSize)
    {
        // arrayElemSize would be truncated as an unsigned char.
        // This means the array element is too large. Don't do the optimization.
        JITDUMP("impArrayAccessIntrinsic: rejecting array intrinsic because arrayElemSize (%d) is too large\n",
                arrayElemSize);
        return nullptr;
    }

    GenTree* val = nullptr;

    if (intrinsicName == NI_Array_Set)
    {
        // Assignment of a struct is more work, and there are more gets than sets.
        // TODO-CQ: support SET (`a[i,j,k] = s`) for struct element arrays.
        if (elemType == TYP_STRUCT)
        {
            JITDUMP("impArrayAccessIntrinsic: rejecting SET array intrinsic because elemType is TYP_STRUCT"
                    " (implementation limitation)\n",
                    arrayElemSize);
            return nullptr;
        }

        val = impPopStack().val;
        assert((genActualType(elemType) == genActualType(val->gtType)) ||
               (elemType == TYP_FLOAT && val->gtType == TYP_DOUBLE) ||
               (elemType == TYP_INT && val->gtType == TYP_BYREF) ||
               (elemType == TYP_DOUBLE && val->gtType == TYP_FLOAT));
    }

    // Here, we're committed to expanding the intrinsic and creating a GT_ARR_ELEM node.
    optMethodFlags |= OMF_HAS_MDARRAYREF;
    compCurBB->bbFlags |= BBF_HAS_MDARRAYREF;

    noway_assert((unsigned char)GT_ARR_MAX_RANK == GT_ARR_MAX_RANK);

    GenTree* inds[GT_ARR_MAX_RANK];
    for (unsigned k = rank; k > 0; k--)
    {
        // The indices should be converted to `int` type, as they would be if the intrinsic was not expanded.
        GenTree* argVal = impPopStack().val;
        if (impInlineRoot()->opts.compJitEarlyExpandMDArrays)
        {
            // This is only enabled when early MD expansion is set because it causes small
            // asm diffs (only in some test cases) otherwise. The GT_ARR_ELEM lowering code "accidentally" does
            // this cast, but the new code requires it to be explicit.
            argVal = impImplicitIorI4Cast(argVal, TYP_INT);
        }
        inds[k - 1] = argVal;
    }

    GenTree* arr = impPopStack().val;
    assert(arr->gtType == TYP_REF);

    GenTree* arrElem = new (this, GT_ARR_ELEM) GenTreeArrElem(TYP_BYREF, arr, static_cast<unsigned char>(rank),
                                                              static_cast<unsigned char>(arrayElemSize), &inds[0]);

    if (intrinsicName != NI_Array_Address)
    {
        if (varTypeIsStruct(elemType))
        {
            arrElem = gtNewObjNode(sig->retTypeClass, arrElem);
        }
        else
        {
            arrElem = gtNewOperNode(GT_IND, elemType, arrElem);
        }
    }

    if (intrinsicName == NI_Array_Set)
    {
        assert(val != nullptr);
        return gtNewAssignNode(arrElem, val);
    }
    else
    {
        return arrElem;
    }
}

//------------------------------------------------------------------------
// impKeepAliveIntrinsic: Import the GC.KeepAlive intrinsic call
//
// Imports the intrinsic as a GT_KEEPALIVE node, and, as an optimization,
// if the object to keep alive is a GT_BOX, removes its side effects and
// uses the address of a local (copied from the box's source if needed)
// as the operand for GT_KEEPALIVE. For the BOX optimization, if the class
// of the box has no GC fields, a GT_NOP is returned.
//
// Arguments:
//    objToKeepAlive - the intrinisic call's argument
//
// Return Value:
//    The imported GT_KEEPALIVE or GT_NOP - see description.
//
GenTree* Compiler::impKeepAliveIntrinsic(GenTree* objToKeepAlive)
{
    assert(objToKeepAlive->TypeIs(TYP_REF));

    if (opts.OptimizationEnabled() && objToKeepAlive->IsBoxedValue())
    {
        CORINFO_CLASS_HANDLE boxedClass = lvaGetDesc(objToKeepAlive->AsBox()->BoxOp()->AsLclVar())->lvClassHnd;
        ClassLayout*         layout     = typGetObjLayout(boxedClass);

        if (!layout->HasGCPtr())
        {
            gtTryRemoveBoxUpstreamEffects(objToKeepAlive, BR_REMOVE_AND_NARROW);
            JITDUMP("\nBOX class has no GC fields, KEEPALIVE is a NOP");

            return gtNewNothingNode();
        }

        GenTree* boxSrc = gtTryRemoveBoxUpstreamEffects(objToKeepAlive, BR_REMOVE_BUT_NOT_NARROW);
        if (boxSrc != nullptr)
        {
            unsigned boxTempNum;
            if (boxSrc->OperIs(GT_LCL_VAR))
            {
                boxTempNum = boxSrc->AsLclVarCommon()->GetLclNum();
            }
            else
            {
                boxTempNum            = lvaGrabTemp(true DEBUGARG("Temp for the box source"));
                GenTree*   boxTempAsg = gtNewTempAssign(boxTempNum, boxSrc);
                Statement* boxAsgStmt = objToKeepAlive->AsBox()->gtCopyStmtWhenInlinedBoxValue;
                boxAsgStmt->SetRootNode(boxTempAsg);
            }

            JITDUMP("\nImporting KEEPALIVE(BOX) as KEEPALIVE(ADDR(LCL_VAR V%02u))", boxTempNum);

            GenTree* boxTemp     = gtNewLclvNode(boxTempNum, boxSrc->TypeGet());
            GenTree* boxTempAddr = gtNewOperNode(GT_ADDR, TYP_BYREF, boxTemp);

            return gtNewKeepAliveNode(boxTempAddr);
        }
    }

    return gtNewKeepAliveNode(objToKeepAlive);
}