/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "renderprocessor.h"

#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include "project/project.h"
#include "rendermanager.h"

OLIVE_NAMESPACE_ENTER

RenderProcessor::RenderProcessor(RenderTicketPtr ticket, Renderer *render_ctx, StillImageCache* still_image_cache, DecoderCache* decoder_cache, ShaderCache *shader_cache) :
  ticket_(ticket),
  render_ctx_(render_ctx),
  still_image_cache_(still_image_cache),
  decoder_cache_(decoder_cache),
  shader_cache_(shader_cache)
{
}

void RenderProcessor::Run()
{
  // Depending on the render ticket type, start a job
  RenderManager::TicketType type = ticket_->property("type").value<RenderManager::TicketType>();

  ticket_->Start();

  switch (type) {
  case RenderManager::kTypeVideo:
  {
    ViewerOutput* viewer = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"));
    rational time = ticket_->property("time").value<rational>();

    NodeValueTable table = ProcessInput(viewer->texture_input(),
                                        TimeRange(time, time + viewer->video_params().time_base()));

    TexturePtr texture = table.Get(NodeParam::kTexture).value<TexturePtr>();

    VideoParams frame_params = viewer->video_params();

    QSize frame_size = ticket_->property("size").value<QSize>();
    if (!frame_size.isNull()) {
      frame_params.set_width(frame_size.width());
      frame_params.set_height(frame_size.height());
    }

    FramePtr frame = Frame::Create();
    frame->set_timestamp(time);
    frame->set_video_params(frame_params);
    frame->allocate();

    if (!texture) {
      // Blank frame out
      memset(frame->data(), 0, frame->allocated_size());
    } else {
      // Dump texture contents to frame
      const VideoParams& tex_params = texture->params();

      if (tex_params.width() != frame->width() || tex_params.height() != frame->height()) {
        // FIXME: Blit this shit
      }

      render_ctx_->DownloadFromTexture(texture.get(), frame->data(), frame->linesize_pixels());
    }

    ticket_->Finish(QVariant::fromValue(frame), IsCancelled());
    break;
  }
  case RenderManager::kTypeAudio:
  {
    ViewerOutput* viewer = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"));
    TimeRange time = ticket_->property("time").value<TimeRange>();

    NodeValueTable table = ProcessInput(viewer->samples_input(), time);

    ticket_->Finish(table.Get(NodeParam::kSamples), IsCancelled());
    break;
  }
  case RenderManager::kTypeVideoDownload:
  {
    FrameHashCache* cache = Node::ValueToPtr<FrameHashCache>(ticket_->property("cache"));
    FramePtr frame = ticket_->property("frame").value<FramePtr>();
    QByteArray hash = ticket_->property("hash").toByteArray();

    ticket_->Finish(cache->SaveCacheFrame(hash, frame), false);
    break;
  }
  default:
    // Fail
    ticket_->Cancel();
  }
}

DecoderPtr RenderProcessor::ResolveDecoderFromInput(StreamPtr stream)
{
  if (!stream) {
    qWarning() << "Attempted to resolve the decoder of a null stream";
    return nullptr;
  }

  QMutexLocker locker(decoder_cache_->mutex());

  DecoderPtr decoder = decoder_cache_->value(stream.get());

  if (!decoder) {
    // No decoder
    decoder = Decoder::CreateFromID(stream->footage()->decoder());

    if (decoder->Open(stream)) {
      decoder_cache_->insert(stream.get(), decoder);
    } else {
      qWarning() << "Failed to open decoder for" << stream->footage()->filename()
                 << "::" << stream->index();
      return nullptr;
    }
  }

  return decoder;
}

void RenderProcessor::Process(RenderTicketPtr ticket, Renderer *render_ctx, StillImageCache *still_image_cache, DecoderCache *decoder_cache, ShaderCache *shader_cache)
{
  RenderProcessor p(ticket, render_ctx, still_image_cache, decoder_cache, shader_cache);
  p.Run();
}

NodeValueTable RenderProcessor::GenerateBlockTable(const TrackOutput *track, const TimeRange &range)
{
  if (track->track_type() == Timeline::kTrackTypeAudio) {

    const AudioParams& audio_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->audio_params();

    QList<Block*> active_blocks = track->BlocksAtTimeRange(range);

    // All these blocks will need to output to a buffer so we create one here
    SampleBufferPtr block_range_buffer = SampleBuffer::CreateAllocated(audio_params,
                                                                       audio_params.time_to_samples(range.length()));
    block_range_buffer->fill(0);

    NodeValueTable merged_table;

    // Loop through active blocks retrieving their audio
    foreach (Block* b, active_blocks) {
      TimeRange range_for_block(qMax(b->in(), range.in()),
                                qMin(b->out(), range.out()));

      int destination_offset = audio_params.time_to_samples(range_for_block.in() - range.in());
      int max_dest_sz = audio_params.time_to_samples(range_for_block.length());

      // Destination buffer
      NodeValueTable table = GenerateTable(b, range_for_block);
      SampleBufferPtr samples_from_this_block = table.Take(NodeParam::kSamples).value<SampleBufferPtr>();

      if (!samples_from_this_block) {
        // If we retrieved no samples from this block, do nothing
        continue;
      }

      // FIXME: Doesn't handle reversing
      if (b->speed_input()->is_keyframing() || b->speed_input()->is_connected()) {
        // FIXME: We'll need to calculate the speed hoo boy
      } else {
        double speed_value = b->speed_input()->get_standard_value().toDouble();

        if (qIsNull(speed_value)) {
          // Just silence, don't think there's any other practical application of 0 speed audio
          samples_from_this_block->fill(0);
        } else if (!qFuzzyCompare(speed_value, 1.0)) {
          // Multiply time
          samples_from_this_block->speed(speed_value);
        }
      }

      int copy_length = qMin(max_dest_sz, samples_from_this_block->sample_count());

      // Copy samples into destination buffer
      block_range_buffer->set(samples_from_this_block->const_data(), destination_offset, copy_length);

      NodeValueTable::Merge({merged_table, table});
    }

    if (ticket_->property("waveforms").toBool()) {
      // Generate a visual waveform and send it back to the main thread
      AudioVisualWaveform visual_waveform;
      visual_waveform.set_channel_count(audio_params.channel_count());
      visual_waveform.OverwriteSamples(block_range_buffer, audio_params.sample_rate());

      RenderedWaveform waveform_info = {track, visual_waveform, range};
      QVector<RenderedWaveform> waveform_list = ticket_->property("waveforms").value< QVector<RenderedWaveform> >();
      waveform_list.append(waveform_info);
      ticket_->setProperty("waveforms", QVariant::fromValue(waveform_list));
    }

    merged_table.Push(NodeParam::kSamples, QVariant::fromValue(block_range_buffer), track);

    return merged_table;

  } else {
    return NodeTraverser::GenerateBlockTable(track, range);
  }
}

QVariant RenderProcessor::ProcessVideoFootage(StreamPtr stream, const rational &input_time)
{
  TexturePtr value = nullptr;

  // Check the still frame cache. On large frames such as high resolution still images, uploading
  // and color managing them for every frame is a waste of time, so we implement a small cache here
  // to optimize such a situation
  VideoStreamPtr video_stream = std::static_pointer_cast<VideoStream>(stream);
  const VideoParams& video_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->video_params();

  StillImageCache::Entry want_entry = {nullptr,
                                       stream,
                                       ColorProcessor::GenerateID(Node::ValueToPtr<ColorManager>(ticket_->property("colormanager")), video_stream->colorspace(), ColorTransform(OCIO::ROLE_SCENE_LINEAR)),
                                       video_stream->premultiplied_alpha(),
                                       video_params.divider(),
                                       (video_stream->video_type() == VideoStream::kVideoTypeStill) ? 0 : input_time};

  still_image_cache_->mutex()->lock();

  foreach (const StillImageCache::Entry& e, still_image_cache_->entries()) {
    if (StillImageCache::CompareEntryMetadata(want_entry, e)) {
      // Found an exact match of the texture we want in the cache, use it instead of reading it
      // ourselves
      value = e.texture;
      break;
    }
  }

  if (!value) {
    // Failed to find the texture, let's see if it's being generated by another processor
    foreach (const StillImageCache::Entry& e, still_image_cache_->pending()) {
      if (StillImageCache::CompareEntryMetadata(want_entry, e)) {
        // An exact match of this texture is pending, let's wait for it
        while (!value) {
          // FIXME: Hacky way of waiting for other threads
          still_image_cache_->mutex()->unlock();
          QThread::msleep(1);
          still_image_cache_->mutex()->lock();

          value = e.texture;
        }
        break;
      }
    }
  }

  if (value) {
    // Found the texture, we can release the cache now
    still_image_cache_->mutex()->unlock();
  } else {
    // Wasn't in still image cache, so we'll have to retrieve it from the decoder

    // Let other processors know we're getting this texture
    still_image_cache_->PushPending(want_entry);

    still_image_cache_->mutex()->unlock();

    DecoderPtr decoder = ResolveDecoderFromInput(stream);

    if (decoder) {
      FramePtr frame = decoder->RetrieveVideo(input_time,
                                              video_params.divider());

      if (frame) {
        // Return a texture from the derived class
        TexturePtr unmanaged_texture = render_ctx_->CreateTexture(frame->video_params(),
                                                                  frame->data(),
                                                                  frame->linesize_pixels());

        // We convert to our rendering pixel format, since that will always be float-based which
        // is necessary for correct color conversion
        VideoParams managed_params = frame->video_params();
        managed_params.set_format(video_params.format());
        value = render_ctx_->CreateTexture(managed_params);

        qDebug() << "FIXME: Accessing video_stream->colorspace() may cause race conditions";

        ColorManager* color_manager = Node::ValueToPtr<ColorManager>(ticket_->property("colormanager"));
        ColorProcessorPtr processor = ColorProcessor::Create(color_manager,
                                                             video_stream->colorspace(),
                                                             ColorTransform(OCIO::ROLE_SCENE_LINEAR));

        render_ctx_->BlitColorManaged(processor, unmanaged_texture, value.get());

        still_image_cache_->mutex()->lock();

        still_image_cache_->RemovePending(want_entry);

        // Put this into the image cache instead
        want_entry.texture = value;
        still_image_cache_->PushEntry(want_entry);

        still_image_cache_->mutex()->unlock();
      }
    }
  }

  return QVariant::fromValue(value);
}

QVariant RenderProcessor::ProcessAudioFootage(StreamPtr stream, const TimeRange &input_time)
{
  QVariant value;

  DecoderPtr decoder = ResolveDecoderFromInput(stream);

  if (decoder) {
    const AudioParams& audio_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->audio_params();

    SampleBufferPtr frame = decoder->RetrieveAudio(input_time, audio_params, &IsCancelled());

    if (frame) {
      value = QVariant::fromValue(frame);
    }
  }

  return value;
}

QVariant RenderProcessor::ProcessShader(const Node *node, const TimeRange &range, const ShaderJob &job)
{
  Q_UNUSED(range)

  QString full_shader_id = QStringLiteral("%1:%2").arg(node->id(), job.GetShaderID());

  QMutexLocker locker(shader_cache_->mutex());

  QVariant shader = shader_cache_->value(full_shader_id);

  if (shader.isNull()) {
    // Since we have shader code, compile it now
    shader = render_ctx_->CreateNativeShader(node->GetShaderCode(job.GetShaderID()));

    if (shader.isNull()) {
      // Couldn't find or build the shader required
      return QVariant();
    }
  }

  const VideoParams& video_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->video_params();

  TexturePtr destination = render_ctx_->CreateTexture(video_params);

  // Run shader
  render_ctx_->BlitToTexture(shader, job, destination.get());

  return QVariant::fromValue(destination);
}

QVariant RenderProcessor::ProcessSamples(const Node *node, const TimeRange &range, const SampleJob &job)
{
  if (!job.samples() || !job.samples()->is_allocated()) {
    return QVariant();
  }

  SampleBufferPtr output_buffer = SampleBuffer::CreateAllocated(job.samples()->audio_params(), job.samples()->sample_count());
  NodeValueDatabase value_db;

  const AudioParams& audio_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->audio_params();

  for (int i=0;i<job.samples()->sample_count();i++) {
    // Calculate the exact rational time at this sample
    double sample_to_second = static_cast<double>(i) / static_cast<double>(audio_params.sample_rate());

    rational this_sample_time = rational::fromDouble(range.in().toDouble() + sample_to_second);

    // Update all non-sample and non-footage inputs
    NodeValueMap::const_iterator j;
    for (j=job.GetValues().constBegin(); j!=job.GetValues().constEnd(); j++) {
      NodeValueTable value;
      NodeInput* corresponding_input = node->GetInputWithID(j.key());

      if (corresponding_input) {
        value = ProcessInput(corresponding_input, TimeRange(this_sample_time, this_sample_time));
      } else {
        value.Push(j.value(), node);
      }

      value_db.Insert(j.key(), value);
    }

    AddGlobalsToDatabase(value_db, TimeRange(this_sample_time, this_sample_time));

    node->ProcessSamples(value_db,
                         job.samples(),
                         output_buffer,
                         i);
  }

  return QVariant::fromValue(output_buffer);
}

QVariant RenderProcessor::ProcessFrameGeneration(const Node *node, const GenerateJob &job)
{
  FramePtr frame = Frame::Create();

  const VideoParams& video_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->video_params();

  frame->set_video_params(video_params);
  frame->allocate();

  node->GenerateFrame(frame, job);

  TexturePtr texture = render_ctx_->CreateTexture(frame->video_params(),
                                                  frame->data(),
                                                  frame->linesize_pixels());

  texture->set_has_meaningful_alpha(job.GetAlphaChannelRequired());

  return QVariant::fromValue(texture);
}

QVariant RenderProcessor::GetCachedFrame(const Node *node, const rational &time)
{
  if (!ticket_->property("cache").toString().isEmpty()
      && node->id() == QStringLiteral("org.olivevideoeditor.Olive.videoinput")) {
    const VideoParams& video_params = Node::ValueToPtr<ViewerOutput>(ticket_->property("viewer"))->video_params();

    QByteArray hash = RenderManager::Hash(node, video_params, time);

    FramePtr f = FrameHashCache::LoadCacheFrame(ticket_->property("cache").toString(), hash);

    qDebug() << ticket_->property("cache").toString() << hash.toHex();

    if (f) {
      // The cached frame won't load with the correct divider by default, so we enforce it here
      VideoParams p = f->video_params();

      p.set_width(f->width() * video_params.divider());
      p.set_height(f->height() * video_params.divider());
      p.set_divider(video_params.divider());

      f->set_video_params(p);

      qDebug() << "Using cached frame!";

      TexturePtr texture = render_ctx_->CreateTexture(f->video_params(), f->data(), f->linesize_pixels());
      return QVariant::fromValue(texture);
    } else {
      qDebug() << "Not using cached frame because frame is null";
    }
  }

  return QVariant();
}

OLIVE_NAMESPACE_EXIT
