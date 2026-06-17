import fs from 'fs';
import { truetype2gfx } from 'truetype2gfx';

async function main() {
    console.log("Reading font file...");
    const fileBuf = fs.readFileSync('BebasNeue.ttf');
    const blob = new Blob([fileBuf], { type: 'font/ttf' });
    
    console.log("Generating 18pt font...");
    const out18 = await truetype2gfx(blob, 18, 'string');
    fs.writeFileSync('src/BebasNeue18.h', out18);
    
    console.log("Generating 24pt font...");
    const out24 = await truetype2gfx(blob, 24, 'string');
    fs.writeFileSync('src/BebasNeue24.h', out24);
    
    console.log("Generating 40pt font...");
    const out40 = await truetype2gfx(blob, 40, 'string');
    fs.writeFileSync('src/BebasNeue40.h', out40);

    console.log("Generating 80pt font...");
    const out80 = await truetype2gfx(blob, 80, 'string');
    fs.writeFileSync('src/BebasNeue80.h', out80);
    
    console.log("Done.");
}

main().catch(console.error);
