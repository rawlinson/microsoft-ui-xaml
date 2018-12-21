﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"
#include "AnimatedVisualPlayer.h"
#include "AnimatedVisualPlayerAutomationPeer.h"
#include "RuntimeProfiler.h"
#include "SharedHelpers.h"
#include <synchapi.h>
#include <winerror.h>


AnimatedVisualPlayer::AnimationPlay::AnimationPlay(
    AnimatedVisualPlayer& owner,
    float fromProgress,
    float toProgress,
    bool looped)
    : m_owner{ owner }
    , m_fromProgress(fromProgress)
    , m_toProgress(toProgress)
    , m_looped(looped)
{
    // Save the play duration as time.
    // If toProgress is less than fromProgress the animation will wrap around,
    // so the time is calculated as fromProgress..end + start..toProgress.
    auto durationAsProgress = fromProgress > toProgress ? ((1 - fromProgress) + toProgress) : (toProgress - fromProgress);
    // NOTE: this relies on the Duration() being set on the owner.
    m_playDuration = std::chrono::duration_cast<winrt::TimeSpan>(m_owner.Duration() * durationAsProgress);
}

float AnimatedVisualPlayer::AnimationPlay::FromProgress()
{
    return m_fromProgress;
}

void AnimatedVisualPlayer::AnimationPlay::Start()
{
    MUX_ASSERT(!m_controller);

    // If the duration is really short (< 20ms) don't bother trying to animate.
    if (m_playDuration < winrt::TimeSpan{ 20ms })
    {
        // Nothing to play. Jump to the from position.
        // This will have the side effect of completing this play immediately.
        m_owner.SetProgress(m_fromProgress);
        // Do not do anything after calling SetProgress()... the AnimationPlay is destructed already.
        return;
    }
    else 
    {
        // Create an animation to drive the Progress property.
        auto compositor = m_owner.m_progressPropertySet.Compositor();
        auto animation = compositor.CreateScalarKeyFrameAnimation();
        animation.Duration(m_playDuration);
        auto linearEasing = compositor.CreateLinearEasingFunction();

        // Play from fromProgress.
        animation.InsertKeyFrame(0, m_fromProgress);

        // from > to is treated as playing from fromProgress to the end, then playing from
        // the beginning to toProgress. Insert extra keyframes to do that.
        if (m_fromProgress > m_toProgress)
        {
            // Play to the end.
            auto timeToEnd = (1 - m_fromProgress) / ((1 - m_fromProgress) + m_toProgress);
            animation.InsertKeyFrame(timeToEnd, 1, linearEasing);
            // Jump to the beginning.
            animation.InsertKeyFrame(timeToEnd + FLT_EPSILON, 0, linearEasing);
        }

        // Play to toProgress
        animation.InsertKeyFrame(1, m_toProgress, linearEasing);

        if (m_looped)
        {
            animation.IterationBehavior(winrt::AnimationIterationBehavior::Forever);
        }
        else
        {
            animation.IterationBehavior(winrt::AnimationIterationBehavior::Count);
            animation.IterationCount(1);
        }

        // Create a batch so that we can know when the animation finishes. This only
        // works for non-looping animations (the batch completes immediately
        // for looping animations).
        m_batch = m_looped
            ? nullptr
            : compositor.CreateScopedBatch(winrt::CompositionBatchTypes::Animation);

        // Start the animation and get the controller.
        m_owner.m_progressPropertySet.StartAnimation(L"Progress", animation);

        m_controller = m_owner.m_progressPropertySet.TryGetAnimationController(L"Progress");

        if (m_isPaused || m_isPausedBecauseHidden)
        {
            // The play was paused before it was started.
            m_controller.Pause();
        }

        // Set the playback rate.
        auto playbackRate = static_cast<float>(m_owner.PlaybackRate());
        m_controller.PlaybackRate(playbackRate);

        if (playbackRate < 0)
        {
            // Play from end to beginning if playing in reverse.
            m_controller.Progress(1);
        }

        if (m_batch)
        {
            // Subscribe to the batch completed event.
            m_batchCompletedToken = m_batch.Completed([this](winrt::IInspectable const&, winrt::CompositionBatchCompletedEventArgs const&)
            {
                // Complete the play when the batch completes.
                // Do not do anything after calling Complete()... the object is destructed already.
                this->Complete();
            });
            // Indicate that nothing else is going into the batch.
            m_batch.End();
        }

        m_owner.IsPlaying(true);
    }
}

bool AnimatedVisualPlayer::AnimationPlay::IsCurrentPlay()
{
    return &*(m_owner.m_nowPlaying) == this;
}

void AnimatedVisualPlayer::AnimationPlay::SetPlaybackRate(float value)
{
    if (m_controller)
    {
        m_controller.PlaybackRate(value);
    }
}

// Called when the animation is becoming hidden.
void AnimatedVisualPlayer::AnimationPlay::OnHiding()
{
    if (!m_isPausedBecauseHidden)
    {
        m_isPausedBecauseHidden = true;

        // Pause the animation if it's not already paused. 
        // This is necessary to ensure that the animation doesn't
        // keep running and causing DWM to wake up when the animation
        // cannot be seen.
        if (m_controller)
        {
            if (!m_isPaused)
            {
                m_controller.Pause();
            }
        }
    }
}

// Called when the animation was hidden but is now becoming visible.
void AnimatedVisualPlayer::AnimationPlay::OnUnhiding()
{
    if (m_isPausedBecauseHidden)
    {
        m_isPausedBecauseHidden = false;

        // Resume the animation that was paused due to the app being suspended.
        if (m_controller)
        {
            if (!m_isPaused)
            {
                m_controller.Resume();
            }
        }
    }
}

void AnimatedVisualPlayer::AnimationPlay::Pause()
{
    m_isPaused = true;

    if (m_controller)
    {
        if (!m_isPausedBecauseHidden)
        {
            m_controller.Pause();
        }
    }
}

void AnimatedVisualPlayer::AnimationPlay::Resume()
{
    m_isPaused = false;

    if (m_controller)
    {
        if (!m_isPausedBecauseHidden)
        {
            m_controller.Resume();
        }
    }
}

// Completes the play, and unregisters it from the player.
// Do not do anything with this object after calling Complete()... the object is destructed already.
void AnimatedVisualPlayer::AnimationPlay::Complete()
{
    //
    // NOTEs about lifetime (i.e. why we can trust that m_owner is still valid)
    //  The AnimatedVisualPlayer will always outlive the AnimationPlay. This
    //  is because:
    //  1. There is only ever one un-completed AnimationPlay. When a new play
    //     is started the current play is completed.
    //  2. An uncompleted AnimationPlay will be completed when the AnimatedVisualPlayer
    //     is unloaded.
    //  3. Completion as a result of a call to SetProgress is always synchronous and is
    //     called from the AnimatedVisualPlayer.
    //  4. If the batch completion event fires, the AnimatedVisualPlayer must still
    //     alive because if it had been unloaded Complete() would have been called
    //     during the unload which would have unsubscribed from the batch completion
    //     event.
    //    

    // Grab a copy of the pointer so the object stays alive until
    // the method returns.
    auto me = m_owner.m_nowPlaying;

    // Unsubscribe from batch.Completed.
    if (m_batch)
    {
        m_batch.Completed(m_batchCompletedToken);
        m_batchCompletedToken = { 0 };
    }

    // If this play is the one that is currently associated with the player,
    // disassociate it from the player and update the player's IsPlaying property.
    if (IsCurrentPlay())
    {
        // Disconnect from the player.
        m_owner.m_nowPlaying.reset();

        // Update the IsPlaying state. Note that this is done
        // after disconnected so that we won't be reentered.
        m_owner.IsPlaying(false);
    }

    // Allow the play to complete.
    CompleteAwaits();
}

AnimatedVisualPlayer::AnimatedVisualPlayer()
{
    __RP_Marker_ClassById(RuntimeProfiler::ProfId_AnimatedVisualPlayer);

    EnsureProperties();

    auto compositor = winrt::ElementCompositionPreview::GetElementVisual(*this).Compositor();
    m_rootVisual = compositor.CreateSpriteVisual();
    m_progressPropertySet = m_rootVisual.Properties();

    // Set an initial value for the Progress property.
    m_progressPropertySet.InsertScalar(L"Progress", 0);

    // Ensure the content can't render outside the bounds of the element.
    m_rootVisual.Clip(compositor.CreateInsetClip());

    // Subscribe to suspending, resuming, and visibility events so we can pause the animation if it's 
    // definitely not visible.
    m_suspendingRevoker = winrt::Application::Current().Suspending(winrt::auto_revoke, [weakThis{ get_weak() }](
        auto const& /*sender*/,
        auto const& /*e*/ )
    {
        if (auto strongThis = weakThis.get())
        {
            strongThis->OnHiding();
        }
    });

    m_resumingRevoker = winrt::Application::Current().Resuming(winrt::auto_revoke, [weakThis{ get_weak() }](
        auto const& /*sender*/,
        auto const& /*e*/)
    {
        if (auto strongThis = weakThis.get())
        {
            if (winrt::CoreWindow::GetForCurrentThread().Visible())
            {
                strongThis->OnUnhiding();
            }
        }
    });

    m_visibilityChangedRevoker = winrt::CoreWindow::GetForCurrentThread().VisibilityChanged(winrt::auto_revoke, [weakThis{ get_weak() }](
        auto const& /*sender*/,
        auto const& e)
    {
        if (auto strongThis = weakThis.get())
        {
            if (e.Visible())
            {
                // Transition from invisible to visible.
                strongThis->OnUnhiding();
            }
            else 
            {
                // Transition from visible to invisible.
                strongThis->OnHiding();
            }
        }
    });

    // Subscribe to the Loaded/Unloaded events to ensure we unload the animated visual then reload
    // when it is next loaded.
    m_loadedRevoker = Loaded({ this, &AnimatedVisualPlayer::OnLoaded });
    m_unloadedRevoker = Unloaded({ this, &AnimatedVisualPlayer::OnUnloaded });
}

AnimatedVisualPlayer::~AnimatedVisualPlayer()
{
    if (m_nowPlaying != nullptr)
    {
        MUX_FAIL_FAST_MSG("Owner AnimatedVisualPlayer is destroyed, current playing AnimationPlay is not empty!");
    }
}

void AnimatedVisualPlayer::OnLoaded(winrt::IInspectable const& /*sender*/, winrt::RoutedEventArgs const& /*args*/)
{
    //
    // Do initialization here rather than in the constructor because when the
    // constructor is called the outer object is not fully initialized.
    //
    // Any initialization that can call back into the outer object MUST be
    // done here rather than the constructor.
    //
    // Other initialization can be done here too, so rather than having to 
    // guess whether an initialization call calls back into the outer, just 
    // put most of the initialization here.
    //

    // Calls back into the outer - must be done OnLoaded rather than in the constructor.
    winrt::ElementCompositionPreview::SetElementChildVisual(*this, m_rootVisual);

    // Set the background for AnimatedVisualPlayer to ensure it will be visible to
    // hit-testing. XAML does not hit test anything that has a null background.
    // Set here rather than in the constructor so we don't have to worry about it
    // calling back into the outer.
    Background(winrt::SolidColorBrush(winrt::Colors::Transparent()));

    if (m_isUnloaded)
    {
        // Reload the content. 
        // Only do this if the elemnent had been previously unloaded so that
        // first Loaded event doesn't overwrite any state that was set before
        // the event was fired.
        UpdateContent();
        m_isUnloaded = false;
    }
} 

void AnimatedVisualPlayer::OnUnloaded(winrt::IInspectable const& /*sender*/, winrt::RoutedEventArgs const& /*args*/)
{
    m_isUnloaded = true;
    // Remove any content. If we get reloaded the content will get reloaded.
    UnloadContent();
}

void AnimatedVisualPlayer::OnHiding()
{
    if (m_nowPlaying)
    {
        m_nowPlaying->OnHiding();
    }
}

void AnimatedVisualPlayer::OnUnhiding()
{
    if (m_nowPlaying)
    {
        m_nowPlaying->OnUnhiding();
    }
}

// IUIElement / IUIElementOverridesHelper
winrt::AutomationPeer AnimatedVisualPlayer::OnCreateAutomationPeer()
{
    return winrt::make<AnimatedVisualPlayerAutomationPeer>(*this);
}

// Overrides FrameworkElement::MeasureOverride. Returns the size that is needed to display the
// animated visual within the available size and respecting the Stretch property.
winrt::Size AnimatedVisualPlayer::MeasureOverride(winrt::Size const& availableSize)
{
    if (m_isFallenBack && Children().Size() > 0)
    {
        // We are showing the fallback content due to a failure to load an animated visual.
        // Tell the content to measure itself.
        Children().GetAt(0).Measure(availableSize);
        // Our size is whatever the fallback content desires.
        return Children().GetAt(0).DesiredSize();
    }

    if ((!m_animatedVisualRoot) || (m_animatedVisualSize == winrt::float2::zero()))
    {
        return { 0, 0 };
    }

    switch (Stretch())
    {
    case winrt::Stretch::None:
        // No scaling will be done. Measured size is the smallest of each dimension.
        return { std::min(m_animatedVisualSize.x, availableSize.Width), std::min(m_animatedVisualSize.y, availableSize.Height) };
    case winrt::Stretch::Fill:
        // Both height and width will be scaled to fill the available space.
        if (availableSize.Width != std::numeric_limits<double>::infinity() && availableSize.Height != std::numeric_limits<double>::infinity())
        {
            // We will scale both dimensions to fill all available space.
            return availableSize;
        }
        // One of the dimensions is infinite and we can't fill infinite dimensions, so
        // fall back to Uniform so at least the non-infinite dimension will be filled.
        break;
    case winrt::Stretch::UniformToFill:
        // Height and width will be scaled by the same amount such that there is no space
        // around the edges.
        if (availableSize.Width != std::numeric_limits<double>::infinity() && availableSize.Height != std::numeric_limits<double>::infinity())
        {
            // Scale so there is no space around the edge.
            auto widthScale = availableSize.Width / m_animatedVisualSize.x;
            auto heightScale = availableSize.Height / m_animatedVisualSize.y;
            auto measuredSize = (heightScale < widthScale)
                ? winrt::Size{ availableSize.Width, m_animatedVisualSize.y * widthScale }
            : winrt::Size{ m_animatedVisualSize.x * heightScale, availableSize.Height };

            // Clip the size to the available size.
            measuredSize = winrt::Size{
                            std::min(measuredSize.Width, availableSize.Width),
                            std::min(measuredSize.Height, availableSize.Height)
            };

            return measuredSize;
        }
        // One of the dimensions is infinite and we can't fill infinite dimensions, so
        // fall back to Uniform so at least the non-infinite dimension will be filled.
        break;
    } // end switch

        // Uniform scaling.
        // Scale so that one dimension fits exactly and no dimension exceeds the boundary.
    auto widthScale = ((availableSize.Width == std::numeric_limits<double>::infinity()) ? FLT_MAX : availableSize.Width) / m_animatedVisualSize.x;
    auto heightScale = ((availableSize.Height == std::numeric_limits<double>::infinity()) ? FLT_MAX : availableSize.Height) / m_animatedVisualSize.y;
    return (heightScale > widthScale)
        ? winrt::Size{ availableSize.Width, m_animatedVisualSize.y * widthScale }
        : winrt::Size{ m_animatedVisualSize.x * heightScale, availableSize.Height };
}

// Overrides FrameworkElement::ArrangeOverride. Scales to fit the animated visual into finalSize 
// respecting the current Stretch and returns the size actually used.
winrt::Size AnimatedVisualPlayer::ArrangeOverride(winrt::Size const& finalSize)
{
    if (m_isFallenBack && Children().Size() > 0)
    {
        // We are showing the fallback content due to a failure to load an animated visual.
        // Tell the content to arrange itself.
        Children().GetAt(0).Arrange(winrt::Rect{ winrt::Point{0,0}, finalSize });
        return finalSize;
    }

    winrt::float2 scale;
    winrt::float2 arrangedSize;

    if (!m_animatedVisualRoot)
    {
        // No content. 0 size.
        scale = { 1, 1 };
        arrangedSize = { 0,0 };
    }
    else
    {
        auto stretch = Stretch();
        if (stretch == winrt::Stretch::None)
        {
            // Do not scale, do not center.
            scale = { 1, 1 };
            arrangedSize = {
                std::min(finalSize.Width, m_animatedVisualSize.x),
                std::min(finalSize.Height, m_animatedVisualSize.y)
            };
        }
        else
        {
            scale = static_cast<winrt::float2>(finalSize) / m_animatedVisualSize;

            switch (stretch)
            {
            case winrt::Stretch::Uniform:
                // Scale both dimensions by the same amount.
                if (scale.x < scale.y)
                {
                    scale.y = scale.x;
                }
                else
                {
                    scale.x = scale.y;
                }
                break;
            case winrt::Stretch::UniformToFill:
                // Scale both dimensions by the same amount and leave no gaps around the edges.
                if (scale.x > scale.y)
                {
                    scale.y = scale.x;
                }
                else
                {
                    scale.x = scale.y;
                }
                break;
            }

            // A size needs to be set because there's an InsetClip applied, and without a 
            // size the clip will prevent anything from being visible.
            arrangedSize = {
                std::min(finalSize.Width / scale.x, m_animatedVisualSize.x),
                std::min(finalSize.Height / scale.y, m_animatedVisualSize.y)
            };

            // Center the animation within the available space.
            auto offset = (finalSize - (m_animatedVisualSize * scale)) / 2;
            auto z = 0.0F;
            m_rootVisual.Offset({ offset, z });

            // Adjust the position of the clip.
            m_rootVisual.Clip().Offset(
                (stretch == winrt::Stretch::UniformToFill)
                ? -(offset / scale)
                : winrt::float2::zero()
            );
        }
    }

    m_rootVisual.Size(arrangedSize);
    auto z = 1.0F;
    m_rootVisual.Scale({ scale, z });

    return finalSize;
}

winrt::DependencyProperty InitializeDp(
    _In_ wstring_view const& propertyNameString,
    _In_ wstring_view const& propertyTypeNameString,
    _In_opt_ winrt::IInspectable defaultValue,
    _In_opt_ winrt::PropertyChangedCallback propertyChangedCallback = nullptr)
{
    // There are no attached properties.
    auto isAttached = false;

    return InitializeDependencyProperty(
        propertyNameString,
        propertyTypeNameString,
        winrt::name_of<winrt::AnimatedVisualPlayer>(),
        isAttached,
        defaultValue,
        propertyChangedCallback);
}

// Accessor for ProgressObject property.
// NOTE: This is not a dependency property because it never changes and is not useful for binding.
winrt::CompositionObject AnimatedVisualPlayer::ProgressObject()
{
    return m_progressPropertySet;
}

// Pauses the currently playing animated visual, or does nothing if no play is underway.
void AnimatedVisualPlayer::Pause()
{
    if (!SharedHelpers::IsRS5OrHigher())
    {
        return;
    }

    if (m_nowPlaying)
    {
        m_nowPlaying->Pause();
    }
}

// Completes the current play, if any.
void AnimatedVisualPlayer::CompleteCurrentPlay()
{
    if (m_nowPlaying)
    {
        m_nowPlaying->Complete();
    }
    MUX_ASSERT(!m_nowPlaying);
}

winrt::IAsyncAction AnimatedVisualPlayer::PlayAsync(double fromProgress, double toProgress, bool looped)
{
    if (!SharedHelpers::IsRS5OrHigher())
    {
        co_return;
    }

    // Used to detect reentrance.
    auto version = ++m_playAsyncVersion;

    // Cause any other plays to return. 
    // This call may cause reentrance.
    Stop();

    if (version != m_playAsyncVersion)
    {
        // The call was overtaken by another call due to reentrance.
        co_return;
    }

    CompleteCurrentPlay();

    // Adjust for the case where there is a segment that
    // goes from [fromProgress..0] where m_fromProgress > 0. 
    // This is equivalent to [fromProgress..1], and by setting
    // toProgress to 1 it saves us from generating extra key frames.
    if (toProgress == 0 && fromProgress > 0)
    {
        toProgress = 1;
    }

    // Adjust for the case where there is a segment that
    // goes from [1..toProgress] where toProgress > 0.
    // This is equivalent to [0..toProgress], and by setting
    // fromProgress to 0 it saves us from generating extra key frames.
    if (toProgress > 0 && fromProgress == 1)
    {
        fromProgress = 0;
    }

    // Create an AnimationPlay to hold the play information.
    // Keep a copy of the pointer because reentrance may cause the m_nowPlaying
    // value to change.
    auto thisPlay = m_nowPlaying = std::make_shared<AnimationPlay>(
        *this,
        std::clamp(static_cast<float>(fromProgress), 0.0F, 1.0F),
        std::clamp(static_cast<float>(toProgress), 0.0F, 1.0F),
        looped);

    if (IsAnimatedVisualLoaded())
    {
        // There is an animated visual loaded, so start it playing.
        thisPlay->Start();
    }

    // Capture the context so we can finish in the calling thread.
    winrt::apartment_context calling_thread;

    // Await the current play. The await will complete when the animation completes
    // or Stop() is called. It can complete on any thread.
    co_await *thisPlay;

    // Get back to the calling thread.
    // This is necessary to destruct the AnimationPlay, and because callers
    // from the dispatcher thread will expect to continue on the dispatcher thread.
    co_await calling_thread;
}

void AnimatedVisualPlayer::Resume()
{
    if (!SharedHelpers::IsRS5OrHigher())
    {
        return;
    }

    if (m_nowPlaying)
    {
        m_nowPlaying->Resume();
    }
}

void AnimatedVisualPlayer::SetProgress(double progress)
{
    if (!SharedHelpers::IsRS5OrHigher())
    {
        return;
    }

    auto clampedProgress = std::clamp(static_cast<float>(progress), 0.0F, 1.0F);

    // Setting the progress value will stop the current play. 
    m_progressPropertySet.InsertScalar(L"Progress", static_cast<float>(clampedProgress));

    // Ensure the current playing task is completed.
    if (m_nowPlaying)
    {
        // Note that this explicit call is necessary, even though InsertScalar
        // will stop the animation, because there will be no BatchCompleted event
        // fired if the play was looped.
        m_nowPlaying->Complete();
    }
}

void AnimatedVisualPlayer::Stop()
{
    if (!SharedHelpers::IsRS5OrHigher())
    {
        return;
    }

    if (m_nowPlaying)
    {
        // Stop the animation by setting the Progress value to the fromProgress of the
        // most recent play.
        SetProgress(m_currentPlayFromProgress);
    }
}

void AnimatedVisualPlayer::OnPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& /*args*/)
{
    // This method is called by AnimatedVisualPlayerProperties::OnPropertyChanged which is auto generated.
    // We have this empty function here in order to make compiler build successfully.
    return;
}

void AnimatedVisualPlayer::OnAutoPlayPropertyChanged(
    winrt::DependencyObject const& sender,
    winrt::DependencyPropertyChangedEventArgs const& args)
{
    auto newValue = unbox_value<bool>(args.NewValue());

    winrt::get_self<AnimatedVisualPlayer>(sender.as<winrt::AnimatedVisualPlayer>())->OnAutoPlayPropertyChanged(newValue);
}


void AnimatedVisualPlayer::OnAutoPlayPropertyChanged(bool newValue)
{
    if (newValue && IsAnimatedVisualLoaded() && !m_nowPlaying)
    {
        // Start playing immediately.
        auto from = 0;
        auto to = 1;
        auto looped = true;
        PlayAsync(from, to, looped);
    }
}

void AnimatedVisualPlayer::OnFallbackContentPropertyChanged(
    winrt::DependencyObject const& sender,
    winrt::DependencyPropertyChangedEventArgs const& args)
{
    winrt::get_self<AnimatedVisualPlayer>(sender.as<winrt::AnimatedVisualPlayer>())->OnFallbackContentPropertyChanged();
}


void AnimatedVisualPlayer::OnFallbackContentPropertyChanged()
{
    if (m_isFallenBack)
    {
        LoadFallbackContent();
    }
}

void AnimatedVisualPlayer::OnSourcePropertyChanged(
    winrt::DependencyObject const& sender,
    winrt::DependencyPropertyChangedEventArgs const& args)
{
    auto oldValue = safe_cast<winrt::IAnimatedVisualSource>(args.OldValue());
    auto newValue = safe_cast<winrt::IAnimatedVisualSource>(args.NewValue());

    winrt::get_self<AnimatedVisualPlayer>(sender.as<winrt::AnimatedVisualPlayer>())->OnSourcePropertyChanged(
        oldValue,
        newValue);
}

void AnimatedVisualPlayer::OnSourcePropertyChanged(winrt::IAnimatedVisualSource const& oldSource, winrt::IAnimatedVisualSource const& newSource)
{
    CompleteCurrentPlay();

    if (auto oldDynamicSource = oldSource.try_as<winrt::IDynamicAnimatedVisualSource>())
    {
        // Disconnect from the update notifications of the old source.
        oldDynamicSource.AnimatedVisualInvalidated(m_dynamicAnimatedVisualInvalidatedToken);
        m_dynamicAnimatedVisualInvalidatedToken = { 0 };
    }

    if (auto newDynamicSource = newSource.try_as<winrt::IDynamicAnimatedVisualSource>())
    {
        // Connect to the update notifications of the new source.
        m_dynamicAnimatedVisualInvalidatedToken = newDynamicSource.AnimatedVisualInvalidated([this](auto const& /*sender*/, auto const&)
        {
            UpdateContent();
        });
    }

    UpdateContent();
}

// Unload the current animated visual (if any).
void AnimatedVisualPlayer::UnloadContent()
{
    // We do not support animated visuals below RS5, so nothing to do.
    if (!SharedHelpers::IsRS5OrHigher())
    {
        return;
    }

    if (m_animatedVisualRoot)
    {
        // This will complete any current play.
        Stop();

        // Remove the old animated visual (if any).
        auto animatedVisual = m_animatedVisual.get();
        if (animatedVisual)
        {
            m_rootVisual.Children().RemoveAll();
            m_animatedVisualRoot = nullptr;
            // Notify the animated visual that it will no longer be used.
            animatedVisual.as<winrt::IClosable>().Close();
            m_animatedVisual.set(nullptr);
        }

        // Size has changed. Tell XAML to re-measure.
        InvalidateMeasure();

        // WARNING - these may cause reentrance.
        Duration(winrt::TimeSpan{ 0 });
        Diagnostics(nullptr);
        IsAnimatedVisualLoaded(false);
    }
}

void AnimatedVisualPlayer::UpdateContent()
{
    // Unload the existing content, if any.
    UnloadContent();

    // Try to create a new animated visual.
    auto source = Source();
    if (!source)
    {
        return;
    }

    winrt::IInspectable diagnostics{};
    auto animatedVisual = source.TryCreateAnimatedVisual(m_rootVisual.Compositor(), diagnostics);
    m_animatedVisual.set(animatedVisual);

    // WARNING - this may cause reentrance.
    Diagnostics(diagnostics);

    if (!animatedVisual)
    {
        // Create failed.

        if (!m_isFallenBack)
        {
            // Show the fallback content, if any.
            m_isFallenBack = true;
            LoadFallbackContent();
        }

        // Complete any play that was started during loading.
        CompleteCurrentPlay();

        return;
    }

    // If the content is empty, do nothing. If we are in fallback from a previous
    // failure to load, stay fallen back.
    // Empty content means the source has nothing to show yet.
    if (!animatedVisual.RootVisual() || animatedVisual.Size() == winrt::float2::zero())
    {
        return;
    }

    // We have non-empty content to show.
    // If we were in fallback, clear that fallback content.
    if (m_isFallenBack)
    {
        // Get out of the fallback state.
        m_isFallenBack = false;
        UnloadFallbackContent();
    }

    // Hook up the new animated visual.
    m_animatedVisualRoot = animatedVisual.RootVisual();
    m_animatedVisualSize = animatedVisual.Size();
    m_rootVisual.Children().InsertAtTop(m_animatedVisualRoot); 
        
    // WARNING - this may cause reentrance
    Duration(animatedVisual.Duration());
    IsAnimatedVisualLoaded(true);

    // Size has changed. Tell XAML to re-measure.
    InvalidateMeasure();

    // Ensure the animated visual has a Progress property. This guarantees that a composition without
    // a Progress property won't blow up when we create an expression that references it below.
    // Normally the animated visual  would have a Progress property that all its expressions reference,
    // but just in case, insert it here.
    m_animatedVisualRoot.Properties().InsertScalar(L"Progress", 0.0F);

    // Tie the animated visual's Progress property to the player Progress with an ExpressionAnimation.
    auto compositor = m_rootVisual.Compositor();
    auto progressAnimation = compositor.CreateExpressionAnimation(L"_.Progress");
    progressAnimation.SetReferenceParameter(L"_", m_progressPropertySet);
    m_animatedVisualRoot.Properties().StartAnimation(L"Progress", progressAnimation);

    if (m_nowPlaying)
    {
        m_nowPlaying->Start();
    }
    else if (AutoPlay())
    {
        // Start playing immediately.
        auto from = 0;
        auto to = 1;
        auto looped = true;
        PlayAsync(from, to, looped);
    }
}

void AnimatedVisualPlayer::LoadFallbackContent()
{
    MUX_ASSERT(m_isFallenBack);

    winrt::UIElement fallbackContentElement{ nullptr };
    auto fallbackContentTemplate = FallbackContent();
    if (fallbackContentTemplate)
    {
        // Load the content from the DataTemplate. It should be a UIElement tree root.
        winrt::DependencyObject fallbackContentObject = fallbackContentTemplate.LoadContent();
        // Get the content.
        fallbackContentElement = safe_cast<winrt::UIElement>(fallbackContentObject);
    }

    // Set the (possibly null) content. We allow null content so as to handle the
    // case where the fallback content got removed - in which case we want to
    // clear out the existing content if any.
    SetFallbackContent(fallbackContentElement);
}

void AnimatedVisualPlayer::UnloadFallbackContent()
{
    MUX_ASSERT(!m_isFallenBack);
    SetFallbackContent(nullptr);
}

void AnimatedVisualPlayer::SetFallbackContent(winrt::UIElement const& uiElement)
{
    // Clear out the existing content.
    Children().Clear();

    // Place the content in the tree.
    if (uiElement)
    {
        Children().Append(uiElement);
    }

    // Size has probably changed. Tell XAML to re-measure.
    InvalidateMeasure();
}

void AnimatedVisualPlayer::OnPlaybackRatePropertyChanged(
    winrt::DependencyObject const& sender,
    winrt::DependencyPropertyChangedEventArgs const& args)
{
    winrt::get_self<AnimatedVisualPlayer>(sender.as<winrt::AnimatedVisualPlayer>())->OnPlaybackRatePropertyChanged(args);
}

void AnimatedVisualPlayer::OnPlaybackRatePropertyChanged(winrt::DependencyPropertyChangedEventArgs const& args)
{
    if (m_nowPlaying)
    {
        m_nowPlaying->SetPlaybackRate(static_cast<float>(unbox_value<double>(args.NewValue())));
    }
}

void AnimatedVisualPlayer::OnStretchPropertyChanged(
    winrt::DependencyObject const& sender,
    winrt::DependencyPropertyChangedEventArgs const&)
{
    winrt::get_self<AnimatedVisualPlayer>(sender.as<winrt::AnimatedVisualPlayer>())->InvalidateMeasure();
}
