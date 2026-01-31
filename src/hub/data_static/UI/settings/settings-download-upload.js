document.addEventListener('DOMContentLoaded', () => {
    loadFileList();
    setupUploadForm();
});

function loadFileList() {
    fetch('/api/settings/files')
        .then(response => response.json())
        .then(data => {
            const fileList = document.getElementById('fileList');
            const targetSelect = document.getElementById('targetFile');
            fileList.innerHTML = '';
            targetSelect.innerHTML = '';

            if (!data.files || data.files.length === 0) {
                fileList.innerHTML = '<div style="color: var(--color-text-secondary);">No files available.</div>';
                return;
            }

            data.files.forEach(file => {
                const row = document.createElement('div');
                row.style.cssText = 'display:flex; align-items:center; justify-content:space-between; gap:1rem; padding:0.75rem 1rem; border:1px solid var(--color-border); border-radius:10px; background:var(--color-bg);';
                row.innerHTML = `
                    <div>
                        <div style="font-weight:600;">${file}</div>
                        <div style="font-size:0.875rem; color:var(--color-text-secondary);">/config/${file}</div>
                    </div>
                    <a class="btn btn-secondary" href="/api/settings/download?name=${encodeURIComponent(file)}">
                        ⬇️ Download
                    </a>
                `;
                fileList.appendChild(row);

                const option = document.createElement('option');
                option.value = file;
                option.textContent = file;
                targetSelect.appendChild(option);
            });
        })
        .catch(error => {
            console.error('Error loading file list:', error);
        });
}

function setupUploadForm() {
    const form = document.getElementById('uploadForm');
    form.addEventListener('submit', (e) => {
        e.preventDefault();

        const fileInput = document.getElementById('uploadFile');
        const targetFile = document.getElementById('targetFile').value;
        const status = document.getElementById('uploadStatus');

        if (!fileInput.files.length) {
            status.innerHTML = '<div style="color: var(--color-accent-danger);">Please select a file.</div>';
            return;
        }

        const formData = new FormData();
        formData.append('file', fileInput.files[0]);

        status.innerHTML = '<div style="color: var(--color-primary);">Uploading...</div>';

        fetch(`/api/settings/upload?target=${encodeURIComponent(targetFile)}`, {
            method: 'POST',
            body: formData
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                status.innerHTML = '<div style="color: var(--color-accent);">Upload complete.</div>';
                fileInput.value = '';
                loadFileList();
            } else {
                status.innerHTML = `<div style="color: var(--color-accent-danger);">${data.error || 'Upload failed'}</div>`;
            }
        })
        .catch(error => {
            console.error('Upload failed:', error);
            status.innerHTML = '<div style="color: var(--color-accent-danger);">Upload failed.</div>';
        });
    });
}
