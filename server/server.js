/* Server ngày 25/04, chạy tốt 
Update ngày 27/04, câu trả lời từ GPT không đầy đủ, cần thêm timeout đợi để thực hiện hết
 */

const express = require('express');
const fs = require('fs');
const fsPromises = require('fs').promises;
const path = require('path');
const OpenAI = require('openai');

const app = express();
const port = 3000;

const resourcesDir = path.resolve('./resources');
const recordFile = path.join(resourcesDir, 'recording.wav');
const voicedFile = path.join(resourcesDir, 'voicedby.wav');

let shouldDownloadFile = false;
const maxTokens = 70;

// Load config và debug
let config;
try {
  config = require('./config');
  console.log('Config loaded:', config);
} catch (error) {
  console.error('Error loading config.js:', error.message);
  throw error;
}

const apiKey = config.apiKey;
console.log('API Key:', apiKey ? 'Loaded' : 'Missing');
if (!apiKey) {
  throw new Error('API Key is missing in config.js');
}

const openai = new OpenAI({ apiKey });

app.use(express.raw({ type: 'audio/wav', limit: '10mb' }));

app.post('/uploadAudio', async (req, res) => {
  try {
    shouldDownloadFile = false;

    console.log('Headers:', req.headers);
    if (!req.body || req.body.length === 0) {
      throw new Error('No audio data received');
    }

    await fsPromises.mkdir(resourcesDir, { recursive: true });
    await fsPromises.unlink(recordFile).catch(() => {});
    await fsPromises.unlink(voicedFile).catch(() => {});

    await fsPromises.writeFile(recordFile, req.body);
    console.log('Received audio file:', recordFile, `Size: ${req.body.length} bytes`);

    const transcription = await speechToTextAPI();
    if (!transcription) {
      throw new Error('Transcription failed');
    }

    await callGPT(transcription);
    res.status(200).send(transcription);
  } catch (error) {
    console.error('Error in /uploadAudio:', error.message);
    res.status(400).send(`Error processing audio: ${error.message}`);
  }
});

app.get('/checkVariable', (req, res) => {
  res.json({ ready: shouldDownloadFile });
});

app.get('/broadcastAudio', async (req, res) => {
  try {
    const stats = await fsPromises.stat(voicedFile);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stats.size,
    });

    const readStream = fs.createReadStream(voicedFile);
    readStream.pipe(res);

    readStream.on('error', (err) => {
      console.error('Error streaming file:', err);
      res.status(500).send('Error streaming audio');
    });
  } catch (error) {
    console.error('File not found:', error);
    res.status(404).send('Audio file not found');
  }
});

app.listen(port, () => {
  console.log(`Server running at http://192.168.1.104:${port}/`);
});

async function speechToTextAPI() {
  try {
    const transcription = await openai.audio.transcriptions.create({
      file: fs.createReadStream(recordFile),
      model: 'whisper-1',
      response_format: 'text',
    });
    console.log('YOU:', transcription);
    return transcription;
  } catch (error) {
    console.error('Error in speechToTextAPI:', error.message);
    return null;
  }
}

async function callGPT(text) {
  try {
    const message = {
      role: 'system',
      content: text,
    };

    const completion = await openai.chat.completions.create({
      messages: [message],
      model: 'gpt-3.5-turbo',
      //max tokens giới hạn số lượng từ, dấu câu, ký tự đặc biệt tối đa 
      //trong phản hồi của chatGPT, một token tương ứng 0.75 từ tiếng anh
      max_tokens: maxTokens,
    });

    const gptResponse = completion.choices[0].message.content;
    console.log('ChatGPT:', gptResponse);
    await gptResponseToSpeech(gptResponse);
  } catch (error) {
    console.error('Error in callGPT:', error.message);
  }
}

async function gptResponseToSpeech(gptResponse) {
  try {
    const wav = await openai.audio.speech.create({
      model: 'tts-1',
      voice: 'echo',
      input: gptResponse,
      response_format: 'wav',
    });

    const buffer = Buffer.from(await wav.arrayBuffer());
    await fsPromises.writeFile(voicedFile, buffer);
    console.log('Audio file saved:', voicedFile);
    shouldDownloadFile = true;
  } catch (error) {
    console.error('Error in gptResponseToSpeech:', error.message);
  }
}